MYDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
cp $MYDIR//build-debug//webdash-server $WEBDASH/app-persistent/bin/_webdash/x64/
echo "Successfully installed newest webdash."
