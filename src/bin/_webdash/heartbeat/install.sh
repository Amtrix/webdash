#!/bin/bash
MYDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
cp "$MYDIR/build/webdash-heartbeat" "$WEBDASH/app-persistent/bin/"
echo "Successfully installed webdash-heartbeat tool."
