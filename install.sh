#!/bin/bash
#
# WebDash install entry point
# ============================
#
# Thin wrapper around .webdash-setup/setup-webdash.sh so setup can be run
# with a simple, discoverable command from the repo root:
#
#   ./install.sh              # Normal setup
#   ./install.sh --test-setup # Sandboxed test run
#
# All arguments are forwarded as-is to the underlying setup script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

exec "$SCRIPT_DIR/.webdash-setup/setup-webdash.sh" "$@"
