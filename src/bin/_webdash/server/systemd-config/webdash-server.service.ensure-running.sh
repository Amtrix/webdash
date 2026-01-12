#!/bin/bash

SERVICE_NAME="webdash-server.service"

if systemctl --user is-active --quiet "$SERVICE_NAME"; then
    echo "$SERVICE_NAME is running (as a user service)."
else
    echo "$SERVICE_NAME is not running (as a user service)."
    webdash $WEBDASH/src/bin/_webdash-server:systemd
fi
