#!/bin/bash

MYDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
rm -f $WEBDASH/app-persistent/bin/webdash
cp $MYDIR//build//webdash $WEBDASH/app-persistent/bin/
echo "Successfully installed newest webdash."
