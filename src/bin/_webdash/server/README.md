# Webdash Server

A background process that can be used to perform routine webdash executions.

## Portal Reachability (Port / Firewall)

The server's portal/static file server listens on the port configured in your profile (commonly `8780`).

If the portal works on `http://127.0.0.1:<port>` but not from another machine (or from Windows when running in WSL), the most common cause is the host firewall.

- **WSL2**: Use WebDash's port helper:
	- `webdash port-forward 8780`
- **Linux (ufw)**:
	- `sudo ufw allow 8780/tcp`
- **Linux (firewalld)**:
	- `sudo firewall-cmd --add-port=8780/tcp --permanent && sudo firewall-cmd --reload`

If you temporarily fix it with `iptables`, remember that rules often don't persist across reboot unless you use something like `iptables-persistent`/`netfilter-persistent` (or migrate to `nftables`).
