  
#/bin/bash

func_localize() {
    local cwd="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"
    local rootdir="$( realpath $cwd/..)"

    printf '\e[1;33m%-6s\e[m\n' "Warning: This setup will erase and re-clone all directories listed in %MYWORLD%/definitions.json"
    su

    export MYWORLD="$rootdir"

    local webdash_lib_dir=$rootdir/src/lib/webdash-executer
    local webdash_client_dir=$rootdir/src/bin/_webdash-client
    declare -a git_urls=()
    declare -a git_destination=()

    git_urls+=(https://github.com/Amtrix/src-lib-webdash-executer)
    git_paths+=($webdash_lib_dir)

    git_urls+=(https://github.com/Amtrix/src-bin-_webdash-client)
    git_paths+=($webdash_client_dir)

    git_urls+=(https://github.com/Amtrix/src-bin-_webdash-server)
    git_paths+=($rootdir/src/bin/_webdash-server)

    git_urls+=(https://github.com/Amtrix/src-bin-report-build-state)
    git_paths+=($rootdir/src/bin/report-build-state)

    git_urls+=(https://github.com/nlohmann/json)
    git_paths+=($rootdir/src/lib/external/json)

    git_urls+=(https://github.com/zaphoyd/websocketpp)
    git_paths+=($rootdir/src/lib/external/websocketpp)

    mkdir -pv "$rootdir/app-temporary"
    mkdir -pv "$rootdir/app-persistent/bin"
    mkdir -pv "$rootdir/app-persistent/lib"

    for index in "${!git_urls[@]}"
    do
        local git_url=${git_urls[index]}
        local path=${git_paths[index]}

        if [ ! -e $path ]
        then
            mkdir -pv "$path"
            cd "$path"
            git clone "$git_url" .
        else
            echo "Already cloned: $git_url -> $path"
        fi
    done

    printf '\e[1;33m%-6s\e[m\n' "Installing packages..."
    sudo add-apt-repository ppa:ubuntu-toolchain-r/test -y > /dev/null
    sudo apt-get update > /dev/null
    sudo apt-get install g++-9 -qq > /dev/null

    sudo apt-get install cmake -qq > /dev/null
    sudo apt-get install make -qq > /dev/null
    sudo ln -sf g++-9 /usr/bin/g++
    sudo apt-get install libboost-dev -qq > /dev/null
    sudo apt-get install libboost-all-dev -qq > /dev/null

    printf '\e[1;33m%-6s\e[m\n' "Building WebDash executer..."
    cd "$webdash_lib_dir"
    mkdir -p build
    cd build
    cmake ../
    make

    printf '\e[1;33m%-6s\e[m\n' "Building WebDash client..."
    cd "$webdash_client_dir"
    mkdir -p build
    cd build
    cmake ../
    make

    printf '\e[1;33m%-6s\e[m\n' "Installing WebDash client..."
    cd ..
    ./install.sh
    cd $rootdir

    printf '\e[1;33m%-6s\e[m\n' "Create bash initialization script for user to source..."
    touch $rootdir/webdash.terminal.init.sh
    echo "# Auto generated" > $rootdir/webdash.terminal.init.sh
    echo "" >> $rootdir/webdash.terminal.init.sh
    echo "$rootdir/app-persistent/bin/webdash create-build-init" >> $rootdir/webdash.terminal.init.sh
    echo "source $rootdir/app-persistent/data/webdash-client/webdash.terminal.init.sh" >> $rootdir/webdash.terminal.init.sh

    printf '\e[1;33m%-6s\e[m\n' "Clone, call :all, and register projects from definitions.json..."
    $rootdir/./app-persistent/bin/webdash _internal_:create-project-cloner
    $rootdir/./app-persistent/data/webdash-client/initialize-projects.sh
}

func_localize