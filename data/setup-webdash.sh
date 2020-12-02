  
#/bin/bash

cwd="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"
rootdir="$( realpath $cwd/..)"
export MYWORLD="$rootdir"
export PATH="$MYWORLD/app-persistent/bin/:$PATH"

func_localize() {
    printf '\e[1;33m%-6s\e[m\n' "Warning: This setup will erase and re-clone all directories listed in %MYWORLD%/definitions.json"

    local webdash_lib_dir=$MYWORLD/src/lib/webdash-executer
    local webdash_client_dir=$MYWORLD/src/bin/_webdash-client
    declare -a git_urls=()
    declare -a git_destination=()

    git_urls+=(https://github.com/Amtrix/src-lib-webdash-executer)
    git_paths+=($webdash_lib_dir)

    git_urls+=(https://github.com/Amtrix/src-bin-_webdash-client)
    git_paths+=($webdash_client_dir)

    git_urls+=(https://github.com/Amtrix/src-bin-_webdash-server)
    git_paths+=($MYWORLD/src/bin/_webdash-server)

    git_urls+=(https://github.com/Amtrix/src-bin-report-build-state)
    git_paths+=($MYWORLD/src/bin/report-build-state)

    git_urls+=(https://github.com/nlohmann/json)
    git_paths+=($MYWORLD/src/lib/external/json)

    git_urls+=(https://github.com/zaphoyd/websocketpp)
    git_paths+=($MYWORLD/src/lib/external/websocketpp)

    mkdir -pv "$MYWORLD/app-temporary"
    mkdir -pv "$MYWORLD/app-persistent/bin"
    mkdir -pv "$MYWORLD/app-persistent/lib"

    local remove_all=false

    for index in "${!git_urls[@]}"
    do
        local git_url=${git_urls[index]}
        local path=${git_paths[index]}

        if [ -e $path ];then
            echo "Already cloned: $git_url -> $path"

            echo "GIT STATUS:"
            cd $path
            git status
            cd $cwd

            if [ "$remove_all" = false ];then
                echo -n "Continuing will ERASE above clone. Continue? (y/n/yes for [A]ll) "
                read yesno < /dev/tty

                if [ "x$yesno" = "xy" ];then
                    rm -rf $path
                elif [ "x$yesno" = "xA" ];then
                    remove_all=true
                    rm -rf $path
                else
                    exit
                fi
            else
                rm -rf $path
            fi
        fi

        mkdir -pv "$path"
        cd "$path"
        git clone "$git_url" .
    done

    printf '\e[1;33m%-6s\e[m\n' "Installing packages..."
    sudo add-apt-repository ppa:ubuntu-toolchain-r/test -y > /dev/null
    sudo apt-get update > /dev/null
    sudo apt-get install g++-9 -qq > /dev/null
    sudo apt-get install gcc-9 -qq > /dev/null

    sudo apt-get install cmake -qq > /dev/null
    sudo apt-get install make -qq > /dev/null
    sudo ln -sf g++-9 /usr/bin/g++
    sudo ln -sf g++-9 /usr/bin/c++
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
    chmod +x install.sh
    ./install.sh
    cd $MYWORLD

    printf '\e[1;33m%-6s\e[m\n' "Create bash initialization script for user to source..."
    touch $MYWORLD/webdash.terminal.init.sh
    echo "# Auto generated. Don't modify." > $MYWORLD/webdash.terminal.init.sh
    echo "" >> $MYWORLD/webdash.terminal.init.sh
    echo "$MYWORLD/app-persistent/bin/webdash _internal_:create-build-init" >> $MYWORLD/webdash.terminal.init.sh
    echo "source $MYWORLD/app-persistent/data/webdash-client/webdash.terminal.init.sh" >> $MYWORLD/webdash.terminal.init.sh

    webdash $MYWORLD/src/bin/_webdash-server:all

    printf '\e[1;33m%-6s\e[m\n' "Clone, call :all, and register projects from definitions.json..."
    $MYWORLD/./app-persistent/bin/webdash _internal_:create-project-cloner
    $MYWORLD/./app-persistent/data/webdash-client/initialize-projects.sh

    printf "\n\n"
    echo "Please add the following two lines to ~/.bashrc:"
    echo "   export MYWORLD=\"$rootdir"
    echo "   export PATH=\"$MYWORLD/app-persistent/bin/:\$PATH\""
    printf "\n\n"
}

func_localize