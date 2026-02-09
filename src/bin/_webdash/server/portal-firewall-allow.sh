#!/usr/bin/env bash
set -euo pipefail

# Allows the WebDash portal/static server port through the WSL/Linux firewall.
# Delegates the actual iptables logic to the shared port-forward tool, so the
# no-op / sudo prompting behavior stays consistent across projects.
#
# Usage:
#   ./portal-firewall-allow.sh [port]
#
# Env overrides:
#   WEBDASH_PORTAL_PORT            Default port if not passed as arg (default: 8780)
#   WEBDASH_PORTAL_HOST_IP         Optional source IP restriction (if set, passed through)
#   WEBDASH_IPTABLES_INSERT_POS    Rule insertion position (default: 3)

PORT="${1:-${WEBDASH_PORTAL_PORT:-8780}}"
POS="${WEBDASH_IPTABLES_INSERT_POS:-3}"
HOST_IP="${WEBDASH_PORTAL_HOST_IP:-}"

if ! [[ "$PORT" =~ ^[0-9]+$ ]] || (( PORT < 1 || PORT > 65535 )); then
    echo "[ERROR] Invalid port: $PORT" >&2
    exit 1
fi

echo "[INFO] Allowing TCP dport $PORT from source $HOST_IP (iptables INPUT insert position $POS)"

WEBDASH_ROOT="${WEBDASH:-}";
if [[ -z "$WEBDASH_ROOT" ]]; then
    WEBDASH_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
fi

SCRIPT="$WEBDASH_ROOT/src/bin/_webdash/port-forward/wsl-port-forward.py"
if [[ ! -f "$SCRIPT" ]]; then
    echo "[ERROR] Port-forward script not found: $SCRIPT" >&2
    exit 1
fi

args=("iptables-allow" "$PORT" "--insert-pos" "$POS")
if [[ -n "$HOST_IP" ]]; then
    args+=("--source-ip" "$HOST_IP")
fi

exec python3 "$SCRIPT" "${args[@]}"
