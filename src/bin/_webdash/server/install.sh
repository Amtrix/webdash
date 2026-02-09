MYDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
mkdir -pv $WEBDASH/app-persistent/bin/_webdash/

TARGET=$WEBDASH/app-persistent/bin/_webdash/webdash-server

# Stop the server if it's running (avoids "Text file busy" on cp)
RESTARTED=false
if systemctl is-active --quiet webdash-server.service 2>/dev/null; then
    echo "Stopping webdash-server.service..."
    sudo systemctl stop webdash-server.service
    RESTARTED=true
fi

rm -f "$TARGET"
cp $MYDIR/build/webdash-server "$TARGET"

# Restart the server if we stopped it
if $RESTARTED; then
    echo "Starting webdash-server.service..."
    sudo systemctl start webdash-server.service
fi

echo "Successfully installed newest webdash-server."
