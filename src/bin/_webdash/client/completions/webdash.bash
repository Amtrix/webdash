# Bash completion for webdash
# Source this file or install to /etc/bash_completion.d/

_webdash_completions() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"

    # All available flags
    local flags="--help -h --config.register.list --config.register.add --config.register.remove --config.register.reload --ping-server --tools.ports.forwarded.list --tools.ports.forwarded.add --tools.ports.forwarded.remove --tools.ports.status --profile.reapply --profile.modules.add --backup.create --backup.restore --config.validate"

    # If current word starts with -, complete flags
    if [[ "$cur" == -* ]]; then
        COMPREPLY=($(compgen -W "$flags" -- "$cur"))
        return
    fi

    # If we're completing the first argument (after 'webdash')
    if [[ ${COMP_CWORD} -eq 1 ]]; then
        local commands=""

        # Try to get commands from local webdash.config.jsonc
        if [[ -f "./webdash.config.jsonc" ]]; then
            # Extract command names from the config file
            commands=$(grep -oP '"name"\s*:\s*"\K[^"]+' ./webdash.config.jsonc 2>/dev/null | tr '\n' ' ')
        fi

        # Combine flags and local commands
        COMPREPLY=($(compgen -W "$flags $commands" -- "$cur"))
        return
    fi
}

# Register the completion function
complete -o default -F _webdash_completions webdash
