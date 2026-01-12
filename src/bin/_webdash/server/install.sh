MYDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
mkdir -pv $WEBDASH/app-persistent/bin/_webdash/

rm -f $WEBDASH/app-persistent/bin/_webdash/webdash-server

cp $MYDIR/build/webdash-server $WEBDASH/app-persistent/bin/_webdash/
echo "Successfully installed newest webdash."