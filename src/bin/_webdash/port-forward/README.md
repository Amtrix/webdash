# WSL Port Forward

Configures Windows and WSL so that a port can be accessed from the local network.
This enables access from other devices on the same WiFi/LAN (e.g., your phone).

## What It Does

1. **WSL iptables**: Adds a rule to allow incoming connections on the port
2. **Windows Firewall**: Adds a rule to allow the port through

That's it! WSL2 automatically forwards traffic via `svchost` - we just need to open the firewalls.

## Usage via WebDash

```bash
# Configure port forwarding (requires sudo + Windows admin)
webdash port-forward 8080

# Remove port forwarding configuration
webdash port-reset 8080

# Show detailed port status
webdash port-whois 8080

# List all configured port forwards
webdash port-list
```

## Direct Usage

```bash
python3 wsl-port-forward.py forward 8080
python3 wsl-port-forward.py reset 8080
python3 wsl-port-forward.py whois 8080
python3 wsl-port-forward.py list
```

## Example

```bash
# Start your server in WSL
python3 -m http.server 8080

# Configure port forwarding
webdash port-forward 8080

# Access from your phone at http://<your-lan-ip>:8080
```

## Requirements

- WSL2 on Windows
- sudo access in WSL (for iptables)
- Windows Administrator privileges (for firewall)
- Python 3.8+
