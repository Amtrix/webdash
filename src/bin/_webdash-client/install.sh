#!/bin/bash

MYDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

# Install the webdash binary
rm -f $WEBDASH/app-persistent/bin/webdash
cp $MYDIR/build/webdash $WEBDASH/app-persistent/bin/

# Install completion scripts
mkdir -p $WEBDASH/app-persistent/data/webdash-client/completions
cp $MYDIR/completions/webdash.bash $WEBDASH/app-persistent/data/webdash-client/completions/

echo "Successfully installed newest webdash."
