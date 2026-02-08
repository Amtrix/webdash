#!/usr/bin/env bash
# Registers "webdash.com" in the hosts file(s) pointing to 127.0.0.1.
# Handles:
#   - Linux /etc/hosts
#   - Windows hosts file (if running under WSL)
# Idempotent: safe to run multiple times.

set -e

HOSTNAME="webdash.com"
IP="127.0.0.1"
MARKER="# managed by webdash"

#
# Update a single hosts file. Handles all common formats:
#   - "127.0.0.1 webdash.com"
#   - "127.0.0.1\twebdash.com"
#   - "127.0.0.1  webdash.com  other-alias"
#   - Entry present but with wrong IP
#   - Entry commented out
#
update_hosts_file() {
    local file="$1"
    local needs_sudo="$2"

    if [[ ! -f "$file" ]]; then
        echo "  ✗ File not found: $file"
        return 1
    fi

    # Check if an uncommented entry for our hostname already exists
    # Match lines where the hostname appears as a whole word (not as part of another name)
    if grep -qE "^[^#]*[[:space:]]${HOSTNAME}([[:space:]]|$)" "$file" 2>/dev/null; then
        # Entry exists - check if it points to the right IP
        if grep -qE "^${IP}[[:space:]].*[[:space:]]?${HOSTNAME}([[:space:]]|$)" "$file" 2>/dev/null; then
            echo "  ✓ $HOSTNAME already resolves to $IP in $file"
            return 0
        else
            # Wrong IP - remove old entry and add correct one
            echo "  Updating $HOSTNAME entry (wrong IP)..."
            local tmp
            tmp=$(mktemp)
            # Remove any line that has our hostname (as a whole word, not commented)
            sed -E "/^[^#]*[[:space:]]${HOSTNAME}([[:space:]]|\$)/d" "$file" > "$tmp"
            echo "${IP}    ${HOSTNAME}    ${MARKER}" >> "$tmp"
            if [[ "$needs_sudo" == "yes" ]]; then
                sudo cp "$tmp" "$file"
            else
                cp "$tmp" "$file"
            fi
            rm -f "$tmp"
            echo "  ✓ Updated $HOSTNAME → $IP in $file"
            return 0
        fi
    fi

    # No entry exists - append it
    echo "  Adding $HOSTNAME → $IP to $file..."
    local entry="${IP}    ${HOSTNAME}    ${MARKER}"
    if [[ "$needs_sudo" == "yes" ]]; then
        echo "$entry" | sudo tee -a "$file" > /dev/null
    else
        echo "$entry" >> "$file"
    fi
    echo "  ✓ Added $HOSTNAME → $IP to $file"
}


echo "Registering $HOSTNAME → $IP"
echo ""

# --- Linux /etc/hosts ---
echo "[1/2] Linux /etc/hosts"
update_hosts_file "/etc/hosts" "yes"
echo ""

# --- Windows hosts file (WSL only) ---
echo "[2/2] Windows hosts file"

if grep -qi microsoft /proc/version 2>/dev/null; then
    # We're in WSL - find Windows hosts file
    WIN_HOSTS=""

    # Try the standard path via wslpath
    if command -v wslpath &>/dev/null; then
        WIN_HOSTS=$(wslpath 'C:\Windows\System32\drivers\etc\hosts' 2>/dev/null) || true
    fi

    # Fallback: try common mount points
    if [[ -z "$WIN_HOSTS" || ! -f "$WIN_HOSTS" ]]; then
        for prefix in /mnt/c /c; do
            candidate="${prefix}/Windows/System32/drivers/etc/hosts"
            if [[ -f "$candidate" ]]; then
                WIN_HOSTS="$candidate"
                break
            fi
        done
    fi

    if [[ -n "$WIN_HOSTS" && -f "$WIN_HOSTS" ]]; then
        # Windows hosts file needs admin privileges - write via PowerShell
        # Check if entry already exists
        if grep -qE "^[^#]*[[:space:]]${HOSTNAME}([[:space:]]|$)" "$WIN_HOSTS" 2>/dev/null; then
            if grep -qE "^${IP}[[:space:]].*[[:space:]]?${HOSTNAME}([[:space:]]|$)" "$WIN_HOSTS" 2>/dev/null; then
                echo "  ✓ $HOSTNAME already resolves to $IP in Windows hosts"
            else
                echo "  Updating $HOSTNAME entry in Windows hosts (wrong IP)..."
                # Use PowerShell with admin to update
                ps_script=$(cat <<'PSEOF'
$hostsPath = "$env:SystemRoot\System32\drivers\etc\hosts"
$hostname = "PLACEHOLDER_HOSTNAME"
$ip = "PLACEHOLDER_IP"
$marker = "PLACEHOLDER_MARKER"
$content = Get-Content $hostsPath | Where-Object { $_ -notmatch "\s${hostname}(\s|$)" }
$content += "${ip}    ${hostname}    ${marker}"
$content | Set-Content $hostsPath -Encoding ASCII
Write-Host "Updated"
PSEOF
)
                ps_script="${ps_script//PLACEHOLDER_HOSTNAME/$HOSTNAME}"
                ps_script="${ps_script//PLACEHOLDER_IP/$IP}"
                ps_script="${ps_script//PLACEHOLDER_MARKER/$MARKER}"

                tmp_ps="/tmp/webdash-hosts-update.ps1"
                echo "$ps_script" > "$tmp_ps"
                win_ps=$(wslpath -w "$tmp_ps")
                powershell.exe -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-ExecutionPolicy','Bypass','-File','$win_ps'" 2>/dev/null
                echo "  ✓ Updated $HOSTNAME → $IP in Windows hosts"
            fi
        else
            echo "  Adding $HOSTNAME → $IP to Windows hosts..."
            ps_cmd="Start-Process powershell -Verb RunAs -Wait -ArgumentList '-Command',\"Add-Content -Path \`\"\$env:SystemRoot\\System32\\drivers\\etc\\hosts\`\" -Value '${IP}    ${HOSTNAME}    ${MARKER}' -Encoding ASCII\""
            powershell.exe -Command "$ps_cmd" 2>/dev/null
            echo "  ✓ Added $HOSTNAME → $IP to Windows hosts"
        fi
    else
        echo "  ✗ Windows hosts file not found (not WSL or non-standard mount)"
    fi
else
    echo "  ⊘ Skipped (not running in WSL)"
fi

echo ""
echo "✓ Done. http://webdash.com:8780 should now resolve to localhost."
