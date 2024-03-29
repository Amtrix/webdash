#/bin/bash

cwd="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && pwd )"
rootdir="$( realpath $cwd/..)"
export MYWORLD="$rootdir"
export PATH="$MYWORLD/app-persistent/bin/:$PATH"

func_localize() {
    printf '\e[1;33m%-6s\e[m\n' "Warning: This setup will erase and re-clone all directories listed in %MYWORLD%/webdash-profile.json"
    echo ""

    local webdash_lib_dir=$MYWORLD/src/lib/webdash-executor
    local webdash_client_dir=$MYWORLD/src/bin/_webdash-client
    declare -a git_urls=()
    declare -a git_destination=()
    declare -a git_branch=()

    git_urls+=(git@github.com:Amtrix/src-bin-_webdash-server.git)
    git_paths+=($MYWORLD/src/bin/_webdash-server)
    git_branch+=("")

    git_urls+=(git@github.com:Amtrix/src-bin-report-build-state)
    git_paths+=($MYWORLD/src/bin/report-build-state)
    git_branch+=("")

    git_urls+=(git@github.com:nlohmann/json)
    git_paths+=($MYWORLD/src/lib/external/json)
    git_branch+=("")

    git_urls+=(git@github.com:zaphoyd/websocketpp)
    git_paths+=($MYWORLD/src/lib/external/websocketpp)
    git_branch+=("develop")

    mkdir -pv "$MYWORLD/app-temporary"
    mkdir -pv "$MYWORLD/app-persistent/bin"
    mkdir -pv "$MYWORLD/app-persistent/lib"

    local remove_all=false

    for index in "${!git_urls[@]}"
    do
        local git_url=${git_urls[index]}
        local path=${git_paths[index]}
        local branch=${git_branch[index]}

        if [ "$skip_all" = true ];then
            continue
        fi

        if [ -e $path ];then
            printf '\e[1;33m%-6s\e[m\n' "Already cloned: $git_url -> $path"

            echo "GIT STATUS:"
            cd $path
            git status
            cd $cwd

            if [ "$remove_all" = false ];then
                printf '\e[033;31m%-6s\e[m\n' "Continuing will ERASE above clone. Continue? (y/n/[s]kip/[A]ll/[S]kip all)"
                read yesno < /dev/tty

                if [ "x$yesno" = "xy" ];then
                    rm -rf $path
                elif [ "x$yesno" = "xA" ];then
                    remove_all=true
                    rm -rf $path
                elif [ "x$yesno" = "xs" ];then
                    continue
                elif [ "x$yesno" = "xS" ];then
                    skip_all=true
                    continue
                else
                    exit
                fi
            else
                rm -rf $path
            fi
        fi

        mkdir -pv "$path"
        pushd "$path"

        echo "BRANCH: $branch"
        if [ ! -z "$branch" ]; then
            echo "CLONE $branch INTO $git_url"
            git clone -b $branch $git_url .
        else
            git clone $git_url .
        fi
        popd
    done

    printf '\e[1;33m%-6s\e[m\n' "Building WebDash executor."
    cd "$webdash_lib_dir"
    mkdir -p build
    cd build
    cmake ../
    make

    printf '\e[1;33m%-6s\e[m\n' "Building WebDash client."
    cd "$webdash_client_dir"
    mkdir -p build
    cd build
    cmake ../
    make

    printf '\e[1;33m%-6s\e[m\n' "Installing WebDash client."
    cd ..
    chmod +x install.sh
    ./install.sh
    cd $MYWORLD

    cp -n $MYWORLD/data/webdash-profile.default.json $MYWORLD/webdash-profile.json

    #
    # Generates a file to initialize a user's shell with
    #

    printf '\e[1;33m%-6s\e[m\n' "Create bash initialization script for user to source."
    touch $MYWORLD/webdash.terminal.init.sh
    echo "# Auto generated. Don't modify."                                                  > $MYWORLD/webdash.terminal.init.sh
    echo "# This file references another auto-generated file by the webdash client binary." > $MYWORLD/webdash.terminal.init.sh
    echo ""                                                                                >> $MYWORLD/webdash.terminal.init.sh
    echo "$MYWORLD/app-persistent/bin/webdash _internal_:create-build-init"                >> $MYWORLD/webdash.terminal.init.sh
    echo "source $MYWORLD/app-persistent/data/webdash-client/webdash.terminal.init.sh"     >> $MYWORLD/webdash.terminal.init.sh

    printf '\e[1;33m%-6s\e[m\n' "Installing and Starting WebDash Server."
    webdash $MYWORLD/src/bin/_webdash-server:all || { exit 1; }

    printf '\e[1;33m%-6s\e[m\n' "Clone, call :all, and register projects from webdash-profile.json."
    $MYWORLD/./app-persistent/bin/webdash _internal_:create-project-cloner # || { exit 1; }
    chmod +x $MYWORLD/./app-persistent/data/webdash-client/initialize-projects.sh
    $MYWORLD/./app-persistent/data/webdash-client/initialize-projects.sh # || { exit 1; }

    printf "\n\n"
    echo "Please add the following two lines to ~/.bashrc:"
    echo "   export MYWORLD=\"$rootdir\""
    echo "   export PATH=\"$MYWORLD/app-persistent/bin/:\$PATH\""
    printf "\n\n"
}

func_localize
