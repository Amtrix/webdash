  
#/bin/bash

func_localize() {
    local cwd="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"
    local rootdir="$( realpath $cwd/..)"

    local git_webdash_lib=https://github.com/Amtrix/src-lib-webdash-executer
    local git_webdash_client=https://github.com/Amtrix/src-bin-_webdash-client
    local git_webdash_server=https://github.com/Amtrix/src-bin-_webdash-server
    local git_webdash_built_reporter=https://github.com/Amtrix/src-bin-report-build-state
    local git_json_lib=https://github.com/nlohmann/json
    local git_websocketpp=https://github.com/zaphoyd/websocketpp

    local webdash_lib=$rootdir/src/lib/webdash-executer
    local webdash_client=$rootdir/src/bin/_webdash-client
    local webdash_server=$rootdir/src/bin/_webdash-server
    local webdash_built_reporter=$rootdir/src/bin/report-build-state
    local json_lib=$rootdir/src/lib/external/json
    local websocketpp=$rootdir/src/lib/external/websocketpp

    export MYWORLD="$rootdir"

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

    mkdir -pv "$json_lib"
    cd "$json_lib"
    git clone "$git_json_lib" .

    mkdir -pv "$websocketpp"
    cd "$websocketpp"
    git clone "$git_websocketpp" .

    sudo add-apt-repository ppa:ubuntu-toolchain-r/test
    sudo apt update
    sudo apt install g++-9

    sudo apt-get install cmake
    sudo apt-get install make
    sudo ln -s g++-9 /usr/bin/g++ 
    sudo apt-get install libboost-dev
    sudo apt-get install libboost-all-dev

    cd "$webdash_lib"
    mkdir build
    cd build
    cmake ../
    make

    cd "$webdash_client"
    mkdir build
    cd build
    cmake ../
    make
    cd ..
    ./install.sh
    cd $rootdir

    # Create bash initialization script for user to source.
    touch $rootdir/webdash.terminal.init.sh
    echo "# Auto generated" > $rootdir/webdash.terminal.init.sh
    echo "" >> $rootdir/webdash.terminal.init.sh
    echo "$rootdir/app-persistent/bin/webdash create-build-init" >> $rootdir/webdash.terminal.init.sh
    echo "source $rootdir/app-persistent/data/webdash-client/webdash.terminal.init.sh" >> $rootdir/webdash.terminal.init.sh

    # Create project cloner
    $rootdir/./app-persistent/bin/webdash create-project-puller
    $rootdir/./app-persistent/data/webdash-client/init-projects.sh
}

func_localize