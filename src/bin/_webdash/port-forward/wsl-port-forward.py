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
        result = subprocess.run(
            ["powershell.exe", "-Command",
             "(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '192.168.*' -or $_.IPAddress -like '10.*' } | Select-Object -First 1).IPAddress"],
            capture_output=True, text=True
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except Exception:
        pass
    return None


def run_powershell(cmd: str) -> tuple[int, str, str]:
    """Run a PowerShell command."""
    result = subprocess.run(["powershell.exe", "-Command", cmd], capture_output=True, text=True)
    return result.returncode, result.stdout, result.stderr


def run_admin_powershell_script(script: str) -> tuple[int, str]:
    """Run a PowerShell script with admin privileges."""
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
    result = subprocess.run(["powershell.exe", "-Command", cmd], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def check_iptables_rule(port: int) -> bool:
    """Check if iptables rule exists for the port."""
    result = subprocess.run(
        ["sudo", "iptables", "-C", "INPUT", "-p", "tcp", "--dport", str(port), "-j", "ACCEPT"],
        capture_output=True
    )
    return result.returncode == 0


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
    rule_name = f"WSL-Port-{port}"
    cmd = f"Get-NetFirewallRule -DisplayName '{rule_name}' -ErrorAction SilentlyContinue"
    code, stdout, _ = run_powershell(cmd)
    return bool(stdout.strip())


def check_portproxy(port: int) -> bool:
    """Check if a netsh portproxy rule exists for the port."""
    code, stdout, _ = run_powershell("netsh interface portproxy show v4tov4")
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] == str(port):
            return True
    return False


def add_portproxy(port: int, wsl_ip: str) -> bool:
    """Add netsh portproxy rule to forward LAN traffic to WSL."""
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
    run_admin_powershell_script(script)
    print(f"  Done")

    # Remove portproxy rule
    print("\n[2/3] Removing port proxy...")
    remove_portproxy(port)
    print(f"  Done")

    # Remove iptables rule
    print("\n[3/3] Removing WSL iptables rule...")
    remove_iptables_rule(port)

    print(f"\n✓ Port {port} configuration reset")
    webdash.log.info(f"Reset port forwarding for port {port}")
    return True


def whois_port(port: int) -> None:
    """Print detailed information about what's using a port."""
    lan_ip = get_lan_ip() or "unknown"

    print(f"Port {port} status:")
    print(f"  LAN IP: {lan_ip}")
    print()

    # WSL process
    process_info = get_port_process_info(port)
    if process_info:
        print(f"  WSL Process:")
        print(f"    Name: {process_info.get('process', 'unknown')}")
        print(f"    PID:  {process_info.get('pid', 'unknown')}")
        if 'cmdline' in process_info:
            cmd = process_info['cmdline'][:60] + "..." if len(process_info['cmdline']) > 60 else process_info['cmdline']
            print(f"    Cmd:  {cmd}")
        if 'cwd' in process_info:
            print(f"    CWD:  {process_info['cwd']}")
    else:
        print(f"  WSL: No process listening")
    print()

    # iptables
    has_iptables = check_iptables_rule(port)
    print(f"  iptables:   {'Allowed' if has_iptables else 'Not configured'}")

    # Firewall
    has_firewall = check_firewall_rule(port)
    print(f"  Firewall:   {'Allowed' if has_firewall else 'Not configured'}")

    # Port proxy
    has_portproxy = check_portproxy(port)
    print(f"  Port proxy: {'Configured' if has_portproxy else 'Not configured'}")

    # Overall status
    print()
    if process_info and has_iptables and has_firewall and has_portproxy:
        print(f"  ✓ Accessible at http://{lan_ip}:{port}")
    else:
        missing = []
        if not process_info:
            missing.append("no WSL server")
        if not has_iptables:
            missing.append("iptables")
        if not has_firewall:
            missing.append("firewall")
        if not has_portproxy:
            missing.append("port proxy")
        print(f"  ✗ Not accessible - missing: {', '.join(missing)}")


def list_all() -> None:
    """List all ports with iptables rules configured."""
    print("Configured port forwards:")
    print("  (firewall = Windows inbound rule, proxy = netsh portproxy to WSL)")
    print()

    lan_ip = get_lan_ip() or "unknown"

    # Get ports from iptables rules
    result = subprocess.run(
        ["sudo", "iptables", "-L", "INPUT", "-n", "--line-numbers"],
        capture_output=True, text=True
    )

    ports = []
    for line in result.stdout.splitlines():
        match = re.search(r'tcp dpt:(\d+)', line)
        if match:
            ports.append(int(match.group(1)))

    if not ports:
        print("  No configured port forwards")
        return

    for port in sorted(ports):
        process_info = get_port_process_info(port)
        wsl_proc = process_info.get("process", "-") if process_info else "-"
        has_firewall = "✓" if check_firewall_rule(port) else "✗"
        has_proxy = "✓" if check_portproxy(port) else "✗"

        print(f"  {lan_ip}:{port}")
        print(f"    WSL: {wsl_proc}  firewall:{has_firewall}  proxy:{has_proxy}")
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
"""
    )

    subparsers = parser.add_subparsers(dest="command", required=True)

    forward_parser = subparsers.add_parser("forward", help="Configure port forwarding")
    forward_parser.add_argument("port", type=int, help="Port number")

    reset_parser = subparsers.add_parser("reset", help="Remove port forwarding")
    reset_parser.add_argument("port", type=int, help="Port number")

    whois_parser = subparsers.add_parser("whois", help="Show port status")
    whois_parser.add_argument("port", type=int, help="Port number")

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


if __name__ == "__main__":
    main()
