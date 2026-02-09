#!/usr/bin/env python3
"""
WSL Port Forwarding Utility

Configures Windows and WSL so that a port can be accessed from the local network.
This includes:
- Adding iptables rule in WSL to allow incoming connections
- Configuring Windows Firewall to allow the port
- Starting a PowerShell TCP forwarder that binds to the LAN IP
"""

import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
from pathlib import Path

# Add webdash bindings to path
_script_dir = Path(__file__).resolve().parent
_webdash_bindings = _script_dir.parent.parent.parent / "lib" / "_webdash" / "executor" / "bindings" / "python"
if _webdash_bindings.exists():
    sys.path.insert(0, str(_webdash_bindings))

try:
    import webdash
    webdash.set_project_name("wsl-port-forward")
except ImportError:
    class MockWebdash:
        class log:
            @staticmethod
            def info(msg): print(f"[INFO] {msg}")
            @staticmethod
            def debug(msg): pass
            @staticmethod
            def warn(msg): print(f"[WARN] {msg}")
            @staticmethod
            def error(msg): print(f"[ERROR] {msg}", file=sys.stderr)
    webdash = MockWebdash()


def get_wsl_ip() -> str:
    """Get WSL's IP address."""
    try:
        result = subprocess.run(
            ["ip", "-4", "addr", "show", "eth0"],
            capture_output=True, text=True
        )
        match = re.search(r'inet (\d+\.\d+\.\d+\.\d+)', result.stdout)
        if match:
            return match.group(1)
    except Exception:
        pass
    return "127.0.0.1"


def get_lan_ip() -> str:
    """Get Windows LAN IP address."""
    try:
        ps = resolve_powershell_exe()
        if not ps:
            return None
        result = subprocess.run(
            [ps, "-NoProfile", "-NonInteractive", "-Command",
             "(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '192.168.*' -or $_.IPAddress -like '10.*' } | Select-Object -First 1).IPAddress"],
            capture_output=True, text=True
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except Exception:
        pass
    return None


def resolve_powershell_exe() -> str | None:
    """Resolve a usable PowerShell executable inside WSL.

    In interactive shells, `powershell.exe` is often on PATH. Under systemd units,
    PATH is typically minimal, so we also probe known /mnt/c locations.
    """
    if not is_wsl():
        return None

    for name in ("powershell.exe", "pwsh.exe"):
        resolved = shutil.which(name)
        if resolved:
            return resolved

    candidates = [
        "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
        "/mnt/c/Program Files/PowerShell/7/pwsh.exe",
        "/mnt/c/Program Files/PowerShell/7-preview/pwsh.exe",
    ]
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate
    return None


def has_windows_integration() -> bool:
    """True when running in WSL with Windows binaries available (powershell.exe, netsh)."""
    return resolve_powershell_exe() is not None


def is_wsl() -> bool:
    """Best-effort detection of WSL."""
    try:
        for path in ("/proc/version", "/proc/sys/kernel/osrelease"):
            if not os.path.exists(path):
                continue
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                s = f.read().lower()
                if "microsoft" in s or "wsl" in s:
                    return True
    except Exception:
        return False
    return False


def get_local_ipv4() -> str | None:
    """Get a best-effort local IPv4 address (non-WSL fallback)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 53))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return None


def run_powershell(cmd: str) -> tuple[int, str, str]:
    """Run a PowerShell command."""
    try:
        ps = resolve_powershell_exe()
        if not ps:
            return 127, "", "powershell.exe not found"

        result = subprocess.run([ps, "-NoProfile", "-NonInteractive", "-Command", cmd], capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except FileNotFoundError:
        return 127, "", "powershell.exe not found"


def run_admin_powershell_script(script: str) -> tuple[int, str]:
    """Run a PowerShell script with admin privileges."""
    ps = resolve_powershell_exe()
    if not ps:
        return 127, "powershell.exe not found"

    temp_script = Path("/tmp/wsl-port-forward-admin.ps1")
    win_script = subprocess.run(
        ["wslpath", "-w", str(temp_script)],
        capture_output=True, text=True
    ).stdout.strip()

    temp_script.write_text(script)

    cmd = f'''
    $output = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList '-ExecutionPolicy', 'Bypass', '-File', '{win_script}'
    exit $output.ExitCode
    '''
    result = subprocess.run([ps, "-Command", cmd], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def check_iptables_rule(port: int) -> bool:
    """Check if iptables rule exists for the port."""
    result = subprocess.run(
        ["sudo", "iptables", "-C", "INPUT", "-p", "tcp", "--dport", str(port), "-j", "ACCEPT"],
        capture_output=True
    )
    return result.returncode == 0


def _detect_default_gateway_ip() -> str | None:
    """Best-effort: detect the default gateway (often the host IP in WSL)."""
    try:
        result = subprocess.run(["ip", "route"], capture_output=True, text=True)
        if result.returncode != 0:
            return None
        for line in result.stdout.splitlines():
            # Example: "default via 172.18.32.1 dev eth0"
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "default" and parts[1] == "via":
                return parts[2]
    except Exception:
        return None
    return None


def _iptables_rule_args(port: int, source_ip: str | None) -> list[str]:
    args = ["INPUT", "-p", "tcp"]
    if source_ip:
        args += ["-s", source_ip]
    args += ["--dport", str(port), "-j", "ACCEPT"]
    return args


def check_iptables_rule_v2(port: int, source_ip: str | None = None) -> tuple[bool | None, str]:
    """
    Check whether a specific iptables rule exists.

    Returns (exists, why):
      - exists=True  => rule exists (no change needed)
      - exists=False => rule definitely missing
      - exists=None  => could not determine without prompting for sudo
    """
    rule_args = _iptables_rule_args(port, source_ip)

    # 1) Try without sudo (best no-op path)
    try:
        r = subprocess.run(["iptables", "-C", *rule_args], capture_output=True, text=True)
        if r.returncode == 0:
            return True, "verified without sudo"
        if r.returncode == 1:
            return False, "missing (verified without sudo)"
        # returncode 4 is commonly "permission denied" for non-root
    except FileNotFoundError:
        return None, "iptables not found"
    except Exception:
        pass

    # 2) Try sudo without prompting
    r = subprocess.run(["sudo", "-n", "iptables", "-C", *rule_args], capture_output=True, text=True)
    if r.returncode == 0:
        return True, "verified via sudo -n"
    if r.returncode == 1:
        # Could be missing, or could be sudo denied in some setups. If stderr indicates sudo issues, treat unknown.
        if "password" in (r.stderr or "").lower() or "sudo" in (r.stderr or "").lower():
            return None, "sudo required to verify"
        return False, "missing (verified via sudo -n)"
    if "password" in (r.stderr or "").lower():
        return None, "sudo required to verify"

    return None, "could not verify rule"


def ensure_iptables_allow_rule(port: int,
                              source_ip: str | None = None,
                              insert_pos: int | None = 3) -> bool:
    """Ensure the INPUT accept rule exists; only prompt for sudo if a change is needed."""
    exists, why = check_iptables_rule_v2(port, source_ip)
    if exists is True:
        print(f"✓ iptables already allows TCP {port}" + (f" from {source_ip}" if source_ip else "") + f" ({why})")
        return True
    if exists is False:
        # We know it's missing, add it.
        pass
    else:
        # Unknown: do a definitive sudo check (may prompt) to avoid adding duplicates.
        rule_args = _iptables_rule_args(port, source_ip)
        r = subprocess.run(["sudo", "iptables", "-C", *rule_args], capture_output=True, text=True)
        if r.returncode == 0:
            print(f"✓ iptables already allows TCP {port}" + (f" from {source_ip}" if source_ip else ""))
            return True

    rule_args = _iptables_rule_args(port, source_ip)
    insert_pos = 3 if insert_pos is None else int(insert_pos)
    cmd = ["sudo", "iptables", "-I", "INPUT", str(insert_pos), *rule_args[1:]]
    # Note: rule_args includes INPUT already; when inserting we provide chain then position then rest.
    # Build: sudo iptables -I INPUT <pos> -p tcp [-s ip] --dport <port> -j ACCEPT
    cmd = ["sudo", "iptables", "-I", "INPUT", str(insert_pos)] + rule_args[1:]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        print(f"✓ Added iptables allow rule for TCP {port}" + (f" from {source_ip}" if source_ip else ""))
        print("  Note: This may not persist across reboot unless you use ufw/firewalld/nftables or iptables-persistent.")
        return True

    print(f"✗ Failed to add iptables rule: {result.stderr.strip()}")
    return False


def add_iptables_rule(port: int) -> bool:
    """Add iptables rule to allow incoming connections on the port."""
    if check_iptables_rule(port):
        print(f"  iptables rule already exists for port {port}")
        return True

    result = subprocess.run(
        ["sudo", "iptables", "-I", "INPUT", "-p", "tcp", "--dport", str(port), "-j", "ACCEPT"],
        capture_output=True
    )
    if result.returncode == 0:
        print(f"  iptables rule added for port {port}")
        return True
    else:
        print(f"  Failed to add iptables rule: {result.stderr.decode()}")
        return False


def remove_iptables_rule(port: int) -> bool:
    """Remove iptables rule for the port."""
    if not check_iptables_rule(port):
        print(f"  No iptables rule found for port {port}")
        return True

    result = subprocess.run(
        ["sudo", "iptables", "-D", "INPUT", "-p", "tcp", "--dport", str(port), "-j", "ACCEPT"],
        capture_output=True
    )
    if result.returncode == 0:
        print(f"  iptables rule removed for port {port}")
        return True
    return False


def check_firewall_rule(port: int) -> bool:
    """Check if Windows firewall rule exists."""
    if not has_windows_integration():
        return False
    rule_name = f"WSL-Port-{port}"
    cmd = f"Get-NetFirewallRule -DisplayName '{rule_name}' -ErrorAction SilentlyContinue"
    code, stdout, _ = run_powershell(cmd)
    return bool(stdout.strip())


def check_portproxy(port: int) -> bool:
    """Check if a netsh portproxy rule exists for the port."""
    if not has_windows_integration():
        return False
    code, stdout, _ = run_powershell("netsh interface portproxy show v4tov4")
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == str(port):
            return True
    return False


def add_portproxy(port: int, wsl_ip: str) -> bool:
    """Add netsh portproxy rule to forward LAN traffic to WSL."""
    if not has_windows_integration():
        print("✗ Windows integration not available (powershell.exe not found)")
        return False
    script = f'''netsh interface portproxy delete v4tov4 listenport={port} listenaddress=0.0.0.0 2>$null
netsh interface portproxy add v4tov4 listenport={port} listenaddress=0.0.0.0 connectport={port} connectaddress={wsl_ip}
if ($LASTEXITCODE -eq 0) {{
    Write-Host "Port proxy added: 0.0.0.0:{port} -> {wsl_ip}:{port}"
}} else {{
    Write-Host "Failed to add port proxy"
    exit 1
}}
'''
    code, output = run_admin_powershell_script(script)
    return code == 0


def remove_portproxy(port: int) -> bool:
    """Remove netsh portproxy rule for the port."""
    if not has_windows_integration():
        return True
    script = f'''$existing = netsh interface portproxy show v4tov4 | Select-String "{port}"
if ($existing) {{
    netsh interface portproxy delete v4tov4 listenport={port} listenaddress=0.0.0.0
    Write-Host "Port proxy removed"
}} else {{
    Write-Host "No port proxy found"
}}
'''
    run_admin_powershell_script(script)
    return True


def get_port_process_info(port: int) -> dict | None:
    """Get information about the WSL process using a port."""
    cmd = f"ss -tlnp 'sport = :{port}' 2>/dev/null"
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)

    if "LISTEN" not in result.stdout:
        return None

    info = {"port": port}
    match = re.search(r'users:\(\("([^"]+)",pid=(\d+)', result.stdout)
    if match:
        info["process"] = match.group(1)
        info["pid"] = int(match.group(2))
        try:
            with open(f"/proc/{info['pid']}/cmdline") as f:
                info["cmdline"] = f.read().replace('\0', ' ').strip()
        except:
            pass
        try:
            info["cwd"] = os.readlink(f"/proc/{info['pid']}/cwd")
        except:
            pass
    return info


def is_port_listening(port: int) -> bool:
    """Check if *anything* is listening on a TCP port in WSL (no process info required)."""
    cmd = f"ss -tln 'sport = :{port}' 2>/dev/null"
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return "LISTEN" in (result.stdout or "")


def forward_port(port: int) -> bool:
    """
    Configure port forwarding from Windows LAN to WSL.

    Sets up:
    1. iptables rule in WSL to accept incoming connections
    2. Windows Firewall rule to allow the port
    3. netsh portproxy rule to forward LAN traffic to WSL
    """
    # Check if already fully configured (no-op)
    iptables_ok = check_iptables_rule(port)
    firewall_ok = check_firewall_rule(port)
    portproxy_ok = check_portproxy(port)

    if iptables_ok and firewall_ok and portproxy_ok:
        lan_ip = get_lan_ip()
        print(f"✓ Port {port} already configured at http://{lan_ip}:{port}")
        return True

    lan_ip = get_lan_ip()
    if not lan_ip:
        if not has_windows_integration():
            print("✗ Not running in WSL with Windows integration (powershell.exe not found)")
        else:
            print("✗ Could not detect Windows LAN IP")
        return False

    wsl_ip = get_wsl_ip()
    print(f"Configuring port {port} forwarding...")
    print(f"  LAN IP: {lan_ip}")
    print(f"  WSL IP: {wsl_ip}")

    # Step 1: Add iptables rule in WSL
    print("\n[1/3] Configuring WSL iptables...")
    if not add_iptables_rule(port):
        print("✗ Failed to configure iptables")
        return False

    # Step 2: Add Windows firewall rule
    print("\n[2/3] Configuring Windows Firewall...")
    rule_name = f"WSL-Port-{port}"
    script = f'''
$ErrorActionPreference = "Continue"
Remove-NetFirewallRule -DisplayName "{rule_name}" -ErrorAction SilentlyContinue
New-NetFirewallRule -DisplayName "{rule_name}" -Direction Inbound -Protocol TCP -LocalPort {port} -Action Allow | Out-Null
if ($?) {{
    Write-Host "Firewall rule added: {rule_name}"
}} else {{
    Write-Host "Failed to add firewall rule"
    exit 1
}}
'''
    print("  Requesting administrator privileges...")
    code, output = run_admin_powershell_script(script)
    if code != 0:
        print(f"✗ Failed to configure firewall: {output}")
        return False
    print(f"  Firewall rule added: {rule_name}")

    # Step 3: Add netsh portproxy rule
    print("\n[3/3] Configuring port proxy...")
    if not add_portproxy(port, wsl_ip):
        print("✗ Failed to configure port proxy")
        return False
    print(f"  Port proxy added: 0.0.0.0:{port} -> {wsl_ip}:{port}")

    print(f"\n✓ Port {port} is now accessible at http://{lan_ip}:{port}")
    webdash.log.info(f"Configured port forwarding for port {port}")
    return True


def reset_port(port: int) -> bool:
    """
    Remove port forwarding configuration.
    """
    print(f"Resetting port {port} configuration...")

    # Remove firewall rule
    print("\n[1/3] Removing Windows Firewall rule...")
    rule_name = f"WSL-Port-{port}"
    script = f'''
$ErrorActionPreference = "Continue"
$rule = Get-NetFirewallRule -DisplayName "{rule_name}" -ErrorAction SilentlyContinue
if ($rule) {{
    Remove-NetFirewallRule -DisplayName "{rule_name}"
    Write-Host "Firewall rule removed"
}} else {{
    Write-Host "No firewall rule found"
}}
'''
    if has_windows_integration():
        run_admin_powershell_script(script)
        print(f"  Done")
    else:
        print("  Skipped (no Windows integration)")

    # Remove portproxy rule
    print("\n[2/3] Removing port proxy...")
    if has_windows_integration():
        remove_portproxy(port)
        print(f"  Done")
    else:
        print("  Skipped (no Windows integration)")

    # Remove iptables rule
    print("\n[3/3] Removing WSL iptables rule...")
    remove_iptables_rule(port)

    print(f"\n✓ Port {port} configuration reset")
    webdash.log.info(f"Reset port forwarding for port {port}")
    return True


def whois_port(port: int) -> None:
    """Print detailed information about what's using a port."""
    lan_ip = get_lan_ip() or get_local_ipv4() or "unknown"

    print(f"Port {port} status:")
    print(f"  LAN IP: {lan_ip}")
    print()

    # WSL listener/process
    process_info = get_port_process_info(port)
    listening = is_port_listening(port)
    if not listening:
        print("  WSL: Not listening")
    else:
        # Process details may be unavailable without elevated privileges.
        if process_info and process_info.get("pid"):
            print("  WSL: Listening")
            print(f"    Process: {process_info.get('process', 'unknown')}")
            print(f"    PID:     {process_info.get('pid', 'unknown')}")
            if 'cmdline' in process_info:
                cmd = process_info['cmdline'][:60] + "..." if len(process_info['cmdline']) > 60 else process_info['cmdline']
                print(f"    Cmd:     {cmd}")
            if 'cwd' in process_info:
                print(f"    CWD:     {process_info['cwd']}")
        else:
            print("  WSL: Listening (process info unavailable without sudo)")
    print()

    # iptables (no sudo prompt)
    exists, why = check_iptables_rule_v2(port, source_ip=None)
    if exists is True:
        print(f"  iptables-firewall: Allowed ({why})")
    elif exists is False:
        print(f"  iptables-firewall: Not configured ({why})")
    else:
        print("  iptables-firewall: Unknown (run with sudo to inspect iptables rules)")

    # Firewall
    if has_windows_integration():
        has_firewall = check_firewall_rule(port)
        print(f"  windows-firewall:  {'Allowed' if has_firewall else 'Not configured'}")
    else:
        has_firewall = False
        print("  windows-firewall:  N/A (not running in WSL)")

    # Port proxy
    if has_windows_integration():
        has_portproxy = check_portproxy(port)
        print(f"  windows-to-wsl-proxy: {'Configured' if has_portproxy else 'Not configured'}")
    else:
        has_portproxy = False
        print("  windows-to-wsl-proxy: N/A (not running in WSL)")

    # Overall status
    print()
    if listening and exists is True and has_firewall and has_portproxy:
        print(f"  ✓ Accessible at http://{lan_ip}:{port}")
    else:
        missing = []
        if not listening:
            missing.append("no WSL listener")
        if exists is False:
            missing.append("iptables-firewall")
        if exists is None:
            missing.append("iptables-firewall (unknown)")
        if not has_firewall:
            missing.append("windows-firewall")
        if not has_portproxy:
            missing.append("windows-to-wsl-proxy")
        print(f"  ✗ Not accessible - missing: {', '.join(missing)}")


def list_all() -> None:
    """List ports that are configured for WSL forwarding (Windows firewall and/or portproxy)."""
    print("Configured port forwards:")
    print("  Columns:")
    print("    status: summary of readiness")
    print("    wsl-listener: whether anything is listening on the WSL TCP port")
    print("    iptables-firewall: WSL/Linux iptables INPUT accept rule")
    print("    windows-firewall: Windows inbound rule")
    print("    windows-to-wsl-proxy: Windows netsh portproxy to WSL")
    print()

    windows = has_windows_integration()
    lan_ip = get_lan_ip() or get_local_ipv4() or "unknown"

    def get_listening_tcp_ports() -> set[int]:
        ports: set[int] = set()
        try:
            r = subprocess.run(["ss", "-H", "-ltn"], capture_output=True, text=True)
            if r.returncode != 0 or not r.stdout:
                return ports
            for line in r.stdout.splitlines():
                parts = line.split()
                if len(parts) < 4:
                    continue
                local = parts[3]
                m = re.search(r":(\d+)$", local)
                if not m:
                    continue
                try:
                    ports.add(int(m.group(1)))
                except Exception:
                    continue
        except Exception:
            return ports
        return ports

    def read_iptables_input_rules_no_prompt() -> str:
        # Never prompt for sudo here.
        for cmd in (
            ["iptables", "-S", "INPUT"],
            ["sudo", "-n", "iptables", "-S", "INPUT"],
        ):
            try:
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode == 0 and r.stdout:
                    return r.stdout
            except FileNotFoundError:
                return ""
            except Exception:
                continue
        return ""

    def get_ports_from_windows_firewall() -> set[int]:
        ports: set[int] = set()
        if not windows:
            return ports
        # DisplayName pattern is created by this tool: WSL-Port-<port>
        code, stdout, _ = run_powershell(
            "Get-NetFirewallRule -DisplayName 'WSL-Port-*' -ErrorAction SilentlyContinue | "
            "Select-Object -ExpandProperty DisplayName"
        )
        if code != 0 or not stdout.strip():
            return ports
        for line in stdout.splitlines():
            m = re.search(r"WSL-Port-(\d+)", line.strip())
            if m:
                try:
                    ports.add(int(m.group(1)))
                except Exception:
                    pass
        return ports

    def get_ports_from_portproxy() -> set[int]:
        ports: set[int] = set()
        if not windows:
            return ports
        code, stdout, _ = run_powershell("netsh interface portproxy show v4tov4")
        if code != 0 or not stdout.strip():
            return ports
        for line in stdout.splitlines():
            parts = line.split()
            # Typical table row contains at least: listenaddress listenport connectaddress connectport
            if len(parts) >= 4 and parts[1].isdigit():
                try:
                    ports.add(int(parts[1]))
                except Exception:
                    pass
        return ports

    # "Configured port forwards" primarily reflect Windows configuration.
    # If we're not in WSL (no powershell.exe), fall back to listing iptables-allowed ports
    # (still best-effort, no sudo prompt).
    firewall_ports = get_ports_from_windows_firewall()
    portproxy_ports = get_ports_from_portproxy()
    ports = firewall_ports | portproxy_ports

    listening_ports_all = get_listening_tcp_ports()

    if not ports and not windows:
        print("  Note: Windows integration not available (powershell.exe not found).")
        print("  Showing iptables-allowed TCP ports instead.")
        print()

    # Best-effort iptables visibility without prompting.
    ipt_rules = read_iptables_input_rules_no_prompt()
    ipt_allowed_ports: set[int] = set()
    ipt_unknown = True
    if ipt_rules:
        ipt_unknown = False
        for line in ipt_rules.splitlines():
            if "-p tcp" not in line or "--dport" not in line or "-j ACCEPT" not in line:
                continue
            m = re.search(r"--dport\s+(\d+)", line)
            if m:
                try:
                    ipt_allowed_ports.add(int(m.group(1)))
                except Exception:
                    pass

    if not ports and not windows:
        # Prefer ports that are actually listening to reduce noise.
        if not ipt_unknown:
            listening_ports = {p for p in ipt_allowed_ports if p in listening_ports_all}
            ports = listening_ports if listening_ports else ipt_allowed_ports

    if not ports:
        print("  No configured port forwards")
        return

    if ipt_unknown:
        # Don't prompt. Just tell the user how to get full visibility.
        print("  Note: iptables-firewall is unknown without sudo. Run: sudo webdash --wsl-forwarded-ports-list")
        print()

    for port in sorted(ports):
        listening = port in listening_ports_all
        if windows:
            windows_firewall = "✓" if port in firewall_ports else "✗"
            win_to_wsl_proxy = "✓" if port in portproxy_ports else "✗"
        else:
            windows_firewall = "-"
            win_to_wsl_proxy = "-"

        if ipt_unknown:
            iptables_fw = "?"
        else:
            iptables_fw = "✓" if port in ipt_allowed_ports else "✗"

        if windows_firewall == "✓" and win_to_wsl_proxy == "✓":
            if iptables_fw == "✓":
                status = "active" if listening else "ready (no WSL listener)"
            elif iptables_fw == "✗":
                status = "incomplete (missing iptables-firewall)"
            else:
                status = "unknown (iptables-firewall unreadable without sudo)"
        else:
            status = "incomplete (missing windows config)"

        print(f"  {lan_ip}:{port}")
        print(f"    status: {status}")
        print(f"    wsl-listener:{'✓' if listening else '✗'}  iptables-firewall:{iptables_fw}  windows-firewall:{windows_firewall}  windows-to-wsl-proxy:{win_to_wsl_proxy}")
        print()


def main():
    parser = argparse.ArgumentParser(
        description="WSL Port Forwarding Utility",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s forward 8080       Configure port 8080 for LAN access
  %(prog)s reset 8080         Remove port 8080 configuration
  %(prog)s whois 8080         Show detailed port status
  %(prog)s list               List all configured port forwards
    %(prog)s iptables-allow 8780 Allow TCP port in WSL/Linux iptables only
"""
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    forward_parser = subparsers.add_parser("forward", help="Configure port forwarding")
    forward_parser.add_argument("port", type=int, help="Port number")

    reset_parser = subparsers.add_parser("reset", help="Remove port forwarding")
    reset_parser.add_argument("port", type=int, help="Port number")

    whois_parser = subparsers.add_parser("whois", help="Show port status")
    whois_parser.add_argument("port", type=int, help="Port number")

    ipt_allow = subparsers.add_parser("iptables-allow", help="Allow a TCP port via iptables only (no Windows changes)")
    ipt_allow.add_argument("port", type=int, help="Port number")
    ipt_allow.add_argument("--source-ip", default=None,
                           help="Optional source IP restriction (defaults to detected gateway if omitted)")
    ipt_allow.add_argument("--insert-pos", type=int, default=3,
                           help="iptables INPUT insert position (default: 3)")
    ipt_allow.add_argument("--no-source-restriction", action="store_true",
                           help="Do not add a -s restriction even if a gateway is detected")

    subparsers.add_parser("list", help="List all port forwards")

    args = parser.parse_args()

    if args.command == "forward":
        success = forward_port(args.port)
        sys.exit(0 if success else 1)
    elif args.command == "reset":
        success = reset_port(args.port)
        sys.exit(0 if success else 1)
    elif args.command == "whois":
        whois_port(args.port)
    elif args.command == "list":
        list_all()
    elif args.command == "iptables-allow":
        source_ip = args.source_ip
        if not source_ip and not args.no_source_restriction:
            source_ip = _detect_default_gateway_ip()
        success = ensure_iptables_allow_rule(args.port, source_ip=source_ip, insert_pos=args.insert_pos)
        sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
