#/bin/bash

cwd="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"
rootdir="$( realpath $cwd/..)"
export MYWORLD="$rootdir"
export PATH="$MYWORLD/app-persistent/bin/:$PATH"


printf '\e[1;33m%-6s\e[m\n' "Clone, call :all, and register projects from webdash-profile.json."
$MYWORLD/./app-persistent/bin/webdash _internal_:create-project-cloner # || { exit 1; }
chmod +x $MYWORLD/./app-persistent/data/webdash-client/initialize-projects.sh
$MYWORLD/./app-persistent/data/webdash-client/initialize-projects.sh # || { exit 1; }