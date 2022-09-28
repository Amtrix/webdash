#!/bin/bash

MYDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
rm -f $MYWORLD/app-persistent/bin/webdash
cp $MYDIR//build//webdash $MYWORLD/app-persistent/bin/
echo "Successfully installed newest webdash."