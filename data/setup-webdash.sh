  
#/bin/bash

func_localize() {
    local cwd="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"

    local rootdir="$cwd/.."

    local git_webdash_lib=https://github.com/Amtrix/src-lib-webdash-executer
    local git_webdash_client=https://github.com/Amtrix/src-bin-_webdash-client
    local git_webdash_server=https://github.com/Amtrix/src-bin-_webdash-server
    local git_webdash_built_reporter=https://github.com/Amtrix/src-bin-report-build-state

    local webdash_lib=$rootdir/src/lib/webdash-executer
    local webdash_client=$rootdir/src/bin/_webdash-client
    local webdash_server=$rootdir/src/bin/_webdash-server
    local webdash_built_reporter=$rootdir/src/bin/report-build-state

    mkdir -pv "$rootdir/app-temporary"
    mkdir -pv "$rootdir/app-persistent/bin"
    mkdir -pv "$rootdir/app-persistent/lib"

    mkdir -pv "$webdash_lib"
    cd "$webdash_lib"
    git clone "$git_webdash_lib" .

    mkdir -pv "$webdash_client"
    cd "$webdash_client"
    git clone "$git_webdash_client" .
    
    mkdir -pv "$webdash_server"
    cd "$webdash_server"
    git clone "$git_webdash_server" .
}

func_localize