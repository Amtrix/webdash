#!/bin/bash
#
# WebDash setup
# =============
#
# Bootstraps a fresh WebDash environment:
#   1. Clone external dependencies (libraries, server, etc.)
#   2. Build the core executor library
#   3. Build and install the `webdash` CLI client
#   4. Build and install the server, then enable+start its systemd service
#      (needs sudo; skipped in test mode)
#   5. Generate and wire up the shell init script
#
# Requirements: git, cmake, make, g++ (>= 9), libboost-all-dev, and an SSH key
# configured for GitHub (clones use git@github.com URLs).
#
# Optional (install manually: sudo apt install -y curl unzip iptables):
#   curl, unzip - the default profile downloads the Android SDK with them
#   iptables    - WSL port forwarding via `webdash --tools.ports.forwarded.add <port>`
# Setup warns about any that are missing and carries on.
#
# Usage:
#   ./install.sh                                    # Normal setup
#   ./install.sh --test-setup                       # Sandboxed test run
#   ./.webdash-setup/setup-webdash.sh               # Normal setup (direct)
#   ./.webdash-setup/setup-webdash.sh --test-setup  # Sandboxed test run (direct)
#

set -euo pipefail  # exit on error, undefined vars, and pipe failures

# =============================================================================
# CONFIGURATION
# =============================================================================

# When true, skip system modifications (bashrc, systemd).
WEBDASH_TEST_MODE="${WEBDASH_TEST_MODE:-false}"

# Absolute paths so the script works from any working directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
WEBDASH_ROOT="$(realpath "$SCRIPT_DIR/..")"

# The C++ header is the single source of truth for canonical filenames.
CPP_CONSTANTS_HEADER="$WEBDASH_ROOT/src/lib/_webdash/executor/include/webdash-constants.hpp"

# Read a constant from the C++ header so bash and C++ never drift apart.
extract_cpp_constant() {
    local var_name="$1"
    grep -oP "${var_name}\\[\\] = \"\\K[^\"]+" "$CPP_CONSTANTS_HEADER" || {
        log_error "Failed to extract $var_name from $CPP_CONSTANTS_HEADER"
        exit 1
    }
}

# These will be populated after we define log_error (chicken-and-egg solved below)
PROFILE_FILENAME=""
TERMINAL_INIT_FILENAME=""

# CMake and other build tools read WEBDASH to locate dependencies.
export WEBDASH="$WEBDASH_ROOT"
export PATH="$WEBDASH/app-persistent/bin:$PATH"

# =============================================================================
# UTILITY FUNCTIONS
# =============================================================================

# Colored output for visibility in terminal
log_info()    { printf '\e[1;34m[INFO]\e[m %s\n' "$1"; }
log_warn()    { printf '\e[1;33m[WARN]\e[m %s\n' "$1" >&2; }
log_error()   { printf '\e[1;31m[ERROR]\e[m %s\n' "$1" >&2; }
log_success() { printf '\e[1;32m[OK]\e[m %s\n' "$1"; }

# Show recent errors from the WebDash error log
show_recent_errors() {
    local error_log="$WEBDASH/app-temporary/logging/webdash-client/logging.error.txt"
    if [[ -f "$error_log" ]]; then
        echo ""
        log_info "Recent errors from WebDash log:"
        echo "----------------------------------------"
        tail -20 "$error_log"
        echo "----------------------------------------"
    fi
}

# Check if a command exists
require_command() {
    if ! command -v "$1" &> /dev/null; then
        log_error "Required command '$1' not found. Please install it first."
        exit 1
    fi
}

# Build a CMake project (centralizes build + error handling).
build_cmake_project() {
    local project_dir="$1"
    local project_name="$2"

    log_info "Building $project_name..."

    if [[ ! -d "$project_dir" ]]; then
        log_error "Project directory not found: $project_dir"
        return 1
    fi

    pushd "$project_dir" > /dev/null

    mkdir -p build
    cd build

    if ! cmake ..; then
        log_error "CMake configuration failed for $project_name"
        popd > /dev/null
        return 1
    fi

    if ! make -j"$(nproc)"; then
        log_error "Build failed for $project_name"
        popd > /dev/null
        return 1
    fi

    popd > /dev/null
    log_success "$project_name built successfully"
}

# =============================================================================
# BOOTSTRAP DEPENDENCIES
# =============================================================================

# Clone bootstrap dependencies by parsing webdash.profile.jsonc directly.
# These must be cloned BEFORE webdash can be built (chicken-and-egg problem).

# Strips JSONC comments from a file before line-based parsing: full-line "//"
# comments and "/* ... */" block comments (which may span multiple lines).
# Deliberately does NOT strip a trailing "// comment" that follows real
# content on the same line - the field-extraction below only captures text
# between quotes, so a trailing comment after a value's closing quote/comma
# doesn't affect it anyway, and this avoids ever having to distinguish a real
# comment from "//" inside a value (e.g. a "https://" URL in a "git" field).
strip_jsonc_comments() {
    local input_file="$1"
    sed -e ':a' -e 'N' -e '$!ba' -e 's#/\*[^*]*\*\+\([^/*][^*]*\*\+\)*/##g' "$input_file" \
        | grep -v '^[[:space:]]*//'
}

clone_bootstrap_dependencies() {
    local profile="$WEBDASH/$PROFILE_FILENAME"

    if [[ ! -f "$profile" ]]; then
        log_warn "Profile not found, copying default..."
        cp "$WEBDASH/.webdash-setup/webdash.profile.default.jsonc" "$profile"
    fi

    log_info "Parsing bootstrap dependencies from $PROFILE_FILENAME..."

    # Parse dependencies array from JSON (these are external libs needed to build webdash)
    local in_dependencies=false
    local current_path=""
    local current_git=""
    local current_branch=""

    while IFS= read -r line; do
        # Detect start of dependencies array
        if [[ "$line" == *'"dependencies"'* ]]; then
            in_dependencies=true
            continue
        fi

        # Detect end of dependencies array (line starting with ])
        if [[ "$in_dependencies" == true ]] && [[ "$line" =~ ^[[:space:]]*\] ]]; then
            # Process last entry if exists
            if [[ -n "$current_path" && -n "$current_git" ]]; then
                clone_single_repo "$current_path" "$current_git" "$current_branch"
            fi
            break
        fi

        if [[ "$in_dependencies" == true ]]; then
            # Parse path field - extract value between quotes after "path":
            if [[ "$line" == *'"path"'* ]]; then
                # Save previous entry
                if [[ -n "$current_path" && -n "$current_git" ]]; then
                    clone_single_repo "$current_path" "$current_git" "$current_branch"
                fi
                # Extract the value: "path": "value" -> value
                current_path=$(echo "$line" | sed -n 's/.*"path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
                current_git=""
                current_branch=""
                # Apply $.rootDir() substitution
                current_path="${current_path//\$.rootDir()/$WEBDASH}"
            fi
            # Parse git field
            if [[ "$line" == *'"git"'* ]]; then
                current_git=$(echo "$line" | sed -n 's/.*"git"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            fi
            # Parse branch field
            if [[ "$line" == *'"branch"'* ]]; then
                current_branch=$(echo "$line" | sed -n 's/.*"branch"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            fi
        fi
    done < <(strip_jsonc_comments "$profile")
}

# Applies .webdash-setup/patches/<repo-dir-name>.patch to a cloned dependency,
# if one exists. Idempotent: skips when the patch is already applied.
# Why: e.g. websocketpp upstream still uses Asio APIs removed in Boost >= 1.87.
apply_dependency_patch() {
    local dest_path="$1"
    local patch_file="$WEBDASH/.webdash-setup/patches/$(basename "$dest_path").patch"

    [[ -f "$patch_file" ]] || return 0

    if git -C "$dest_path" apply --check -R "$patch_file" >/dev/null 2>&1; then
        log_info "Patch already applied: $(basename "$patch_file")"
    elif git -C "$dest_path" apply "$patch_file"; then
        log_success "Applied patch: $(basename "$patch_file")"
    else
        log_error "Failed to apply $(basename "$patch_file") to $dest_path"
        return 1
    fi
}

clone_single_repo() {
    local dest_path="$1"
    local git_url="$2"
    local branch="$3"

    if [[ -d "$dest_path" ]]; then
        log_info "Already exists: $dest_path (skipping clone)"
        apply_dependency_patch "$dest_path"
        return $?
    fi

    log_info "Cloning: $git_url"
    if [[ -n "$branch" ]]; then
        log_info "  Branch: $branch"
        if ! git clone -b "$branch" "$git_url" "$dest_path"; then
            log_error "Failed to clone $git_url"
            return 1
        fi
    else
        if ! git clone "$git_url" "$dest_path"; then
            log_error "Failed to clone $git_url"
            return 1
        fi
    fi
    log_success "Cloned: $(basename "$dest_path")"
    apply_dependency_patch "$dest_path"
}

# =============================================================================
# MAIN SETUP FUNCTION
# =============================================================================

main() {
    # -------------------------------------------------------------------------
    # Extract canonical constants from C++ header (single source of truth)
    # Why: Ensures bash and C++ use identical values without duplication.
    # -------------------------------------------------------------------------
    if [[ ! -f "$CPP_CONSTANTS_HEADER" ]]; then
        # Fallback if header doesn't exist yet (first-time setup before clone)
        log_warn "C++ constants header not found, using hardcoded defaults"
        PROFILE_FILENAME="webdash.profile.jsonc"
        TERMINAL_INIT_FILENAME="webdash.terminal.init.sh"
    else
        PROFILE_FILENAME=$(extract_cpp_constant "kRootProfileFilename")
        TERMINAL_INIT_FILENAME=$(extract_cpp_constant "kTerminalInitFilename")
        log_info "Loaded constants from C++ header:"
        log_info "  PROFILE_FILENAME=$PROFILE_FILENAME"
        log_info "  TERMINAL_INIT_FILENAME=$TERMINAL_INIT_FILENAME"
    fi

    log_warn "This setup may erase and re-clone the directories listed above."
    log_info "WebDash root: $WEBDASH"
    echo ""

    # Step 0: Verify prerequisites.
    log_info "Checking prerequisites..."
    require_command git
    require_command cmake
    require_command make
    require_command g++

    # Optional tools: setup continues without them, but the features noted below won't work.
    local missing_optional=()
    command -v curl >/dev/null 2>&1     || missing_optional+=("curl (downloads the Android SDK in the default profile)")
    command -v unzip >/dev/null 2>&1    || missing_optional+=("unzip (unpacks the Android SDK in the default profile)")
    command -v iptables >/dev/null 2>&1 || missing_optional+=("iptables (WSL port forwarding: webdash --tools.ports.forwarded.add)")
    if [[ ${#missing_optional[@]} -gt 0 ]]; then
        log_warn "Optional tools not found:"
        for tool in "${missing_optional[@]}"; do
            log_warn "  - $tool"
        done
        log_warn "Install them manually if you need those features: sudo apt install -y curl unzip iptables"
    fi

    # Step 1: Create the standard directory layout.
    log_info "Creating directory structure..."

    mkdir -p "$WEBDASH/app-temporary"        # logs, caches, temp files (gitignored)
    mkdir -p "$WEBDASH/app-persistent/bin"   # compiled binaries
    mkdir -p "$WEBDASH/app-persistent/lib"   # compiled libraries
    mkdir -p "$WEBDASH/app-persistent/data"  # persistent app data

    # Step 2: Clone external libraries needed to build the executor.
    # (Once webdash is built it can auto-clone modules from the profile.)
    clone_bootstrap_dependencies

    # Step 3: Build the core executor library (client and server depend on it).
    build_cmake_project "$WEBDASH/src/lib/_webdash/executor" "WebDash Executor Library"

    # Step 4: Build and install the `webdash` CLI client.
    build_cmake_project "$WEBDASH/src/bin/_webdash/client" "WebDash Client"

    log_info "Installing WebDash client to app-persistent/bin..."
    pushd "$WEBDASH/src/bin/_webdash/client" > /dev/null
    chmod +x install.sh
    ./install.sh
    popd > /dev/null
    log_success "WebDash client installed"

    # Step 5: Create the profile from the default if one doesn't exist yet.
    if [[ ! -f "$WEBDASH/$PROFILE_FILENAME" ]]; then
        log_info "Creating $PROFILE_FILENAME from default..."
        cp "$WEBDASH/.webdash-setup/webdash.profile.default.jsonc" "$WEBDASH/$PROFILE_FILENAME"
    else
        log_info "$PROFILE_FILENAME already exists, keeping it"
    fi

    # Step 6: Generate the shell init script (sets WEBDASH, PATH, and profile
    # env vars). The webdash client writes the file itself.
    log_info "Generating shell initialization script..."

    local TERMINAL_INIT_PATH="app-persistent/data/webdash-client/$TERMINAL_INIT_FILENAME"

    if ! "$WEBDASH/app-persistent/bin/webdash" _internal_:create-build-init; then
        log_error "Failed to generate shell init script"
        exit 1
    fi

    log_success "Shell init script created: \$WEBDASH/$TERMINAL_INIT_PATH"

    # Step 7: Build the server. It tracks registered configs and runs scheduled
    # tasks. In test mode we only build it; otherwise we install it via systemd.
    if [[ "$WEBDASH_TEST_MODE" == "true" ]]; then
        log_info "[TEST MODE] Building WebDash Server (skipping install/systemd)..."
        if ! "$WEBDASH/app-persistent/bin/webdash" "$WEBDASH/src/bin/_webdash/server:build"; then
            log_error "Failed to build WebDash server"
            show_recent_errors
            exit 1
        fi
        # Verify the binary was actually created
        if [[ ! -f "$WEBDASH/src/bin/_webdash/server/build/webdash-server" ]]; then
            log_error "WebDash server build failed - binary not found"
            show_recent_errors
            exit 1
        fi
        log_success "WebDash server built (not installed in test mode)"
    else
        local webdash_bin="$WEBDASH/app-persistent/bin/webdash"
        local server_proj="$WEBDASH/src/bin/_webdash/server"

        log_info "Building and installing WebDash Server..."
        if ! "$webdash_bin" "$server_proj:all"; then
            log_error "Failed to build/install WebDash server"
            log_warn "You can try manually later: webdash \$WEBDASH/src/bin/_webdash/server:all"
        else
            log_success "WebDash server installed"

            # ':all' only builds + installs the binary. ':systemd' creates, enables and
            # starts the service; it uses sudo, so it may prompt for your password.
            log_info "Installing and starting the WebDash server systemd service (sudo required)..."
            if ! "$webdash_bin" "$server_proj:systemd"; then
                log_error "Failed to install/start the WebDash server systemd service"
                log_warn "You can try manually later: webdash \$WEBDASH/src/bin/_webdash/server:systemd"
            else
                # Confirm the server actually answers before claiming success.
                local server_up=false
                for _ in $(seq 1 10); do
                    if "$webdash_bin" --ping-server >/dev/null 2>&1; then
                        server_up=true
                        break
                    fi
                    sleep 1
                done
                if [[ "$server_up" == true ]]; then
                    log_success "WebDash server is running"

                    # The portal UI is only served once its project is registered.
                    local portal_config="$WEBDASH/src/bin/_webdash/portal-user-interface/webdash.config.jsonc"
                    if [[ -f "$portal_config" ]]; then
                        if "$webdash_bin" --config.register.add "$portal_config" >/dev/null 2>&1; then
                            log_success "Portal registered (served at /portal/ on the static file server port, default 8780)"
                        else
                            log_warn "Could not register the portal. Try: webdash --config.register.add $portal_config"
                        fi
                    fi
                else
                    log_warn "WebDash server service was installed but did not respond to --ping-server"
                    log_warn "Check: systemctl status webdash-server.service"
                fi
            fi
        fi
    fi

    # Step 8: Clone and register projects declared in the profile.
    log_info "Processing projects from $PROFILE_FILENAME..."

    # Generate the clone script from profile
    "$WEBDASH/app-persistent/bin/webdash" _internal_:create-project-cloner 2>/dev/null || true

    local init_script="$WEBDASH/app-persistent/data/webdash-client/initialize-projects.sh"
    if [[ -f "$init_script" ]]; then
        chmod +x "$init_script"
        "$init_script" || log_warn "Some projects failed to initialize (see the list above). Fix the cause, then run: webdash --profile.reapply"
    fi

    # -------------------------------------------------------------------------
    # Done! Configure shell automatically
    # -------------------------------------------------------------------------
    echo ""
    echo "=============================================="
    log_success "WebDash setup complete!"
    echo "=============================================="
    echo ""

    # Skip system modifications in test mode
    if [[ "$WEBDASH_TEST_MODE" == "true" ]]; then
        log_info "[TEST MODE] Skipping shell config modification"
        log_info "[TEST MODE] Skipping shell activation"
        log_success "Test setup finished successfully!"
        return 0
    fi

    # Determine which shell config file to use
    local shell_rc=""
    if [[ -n "${ZSH_VERSION:-}" ]] || [[ "$SHELL" == *"zsh"* ]]; then
        shell_rc="$HOME/.zshrc"
    else
        shell_rc="$HOME/.bashrc"
    fi

    # Lines to add to shell config
    local export_line="export WEBDASH=\"$WEBDASH_ROOT\""
    local source_line="source \"\$WEBDASH/app-persistent/data/webdash-client/$TERMINAL_INIT_FILENAME\""
    local marker_start="# WebDash initialization"
    local marker_end="# WebDash initialization end"

    # Remove existing WebDash config block if present, then add fresh
    if grep -qF "$marker_start" "$shell_rc" 2>/dev/null; then
        log_info "Updating WebDash configuration in $shell_rc..."
        # Remove old block between markers (or just the old lines if no end marker)
        if grep -qF "$marker_end" "$shell_rc" 2>/dev/null; then
            # Remove block between markers (inclusive)
            sed -i "/$marker_start/,/$marker_end/d" "$shell_rc"
        else
            # Legacy: remove old-style lines without end marker
            sed -i "/$marker_start/d" "$shell_rc"
            sed -i "/export WEBDASH=/d" "$shell_rc"
            sed -i "/source.*webdash.*terminal.*init/d" "$shell_rc"
        fi
    fi

    log_info "Adding WebDash initialization to $shell_rc..."
    {
        echo ""
        echo "$marker_start"
        echo "$export_line"
        echo "$source_line"
        echo "$marker_end"
    } >> "$shell_rc"
    log_success "WebDash initialization configured in $shell_rc"

    # Source the shell config to activate WebDash in current session
    log_info "Activating WebDash in current shell..."
    export WEBDASH="$WEBDASH_ROOT"
    source "$WEBDASH/app-persistent/data/webdash-client/$TERMINAL_INIT_FILENAME"
    log_success "WebDash is now active!"
    echo ""
}

# =============================================================================
# ENTRY POINT
# =============================================================================

# -----------------------------------------------------------------------------
# Sandboxed Test Setup
# -----------------------------------------------------------------------------
# Creates an isolated copy of webdash with current uncommitted changes applied,
# then runs setup in that sandbox without modifying the real system.

run_test_setup() {
    local test_dir="$WEBDASH_ROOT/app-temporary/_webdash/test-framework-setup"
    local patch_file="/tmp/webdash-test-$$.patch"

    log_info "=== SANDBOXED TEST SETUP ==="
    log_info "Creating isolated test environment..."

    # Clean up any previous test
    if [[ -d "$test_dir" ]]; then
        log_warn "Removing previous test directory..."
        rm -rf "$test_dir"
    fi

    mkdir -p "$test_dir"

    # Get current branch
    local current_branch
    current_branch=$(git -C "$WEBDASH_ROOT" branch --show-current 2>/dev/null || echo "main")

    log_info "Current branch: $current_branch"
    log_info "Source repo: $WEBDASH_ROOT"

    # Generate patch of uncommitted changes (staged + unstaged, tracked files only)
    log_info "Capturing uncommitted changes..."
    if ! git -C "$WEBDASH_ROOT" diff HEAD > "$patch_file" 2>/dev/null; then
        # No commits yet or other issue, try without HEAD
        git -C "$WEBDASH_ROOT" diff > "$patch_file" 2>/dev/null || true
    fi

    local patch_size
    patch_size=$(wc -c < "$patch_file")
    if [[ "$patch_size" -gt 0 ]]; then
        log_info "Captured $(wc -l < "$patch_file") lines of uncommitted changes"
    else
        log_info "No uncommitted changes to apply"
    fi

    # Get list of untracked files (new files not yet added to git)
    local untracked_files
    untracked_files=$(git -C "$WEBDASH_ROOT" ls-files --others --exclude-standard 2>/dev/null || true)

    # Clone the local repository state so local commits are included
    log_info "Cloning local branch $current_branch into test directory..."
    if ! git clone --branch "$current_branch" --single-branch "$WEBDASH_ROOT" "$test_dir/webdash" 2>&1; then
        log_error "Failed to clone repository"
        rm -f "$patch_file"
        exit 1
    fi

    # Apply the patch if there are changes
    if [[ "$patch_size" -gt 0 ]]; then
        log_info "Applying uncommitted changes to test copy..."
        pushd "$test_dir/webdash" > /dev/null
        if ! git apply "$patch_file" 2>&1; then
            log_warn "Some changes could not be applied cleanly"
            # Try with 3-way merge
            git apply --3way "$patch_file" 2>&1 || true
        fi
        popd > /dev/null
        log_success "Changes applied"
    fi

    rm -f "$patch_file"

    # Copy untracked files to test directory
    if [[ -n "$untracked_files" ]]; then
        log_info "Copying $(echo "$untracked_files" | wc -l) untracked files..."
        while IFS= read -r file; do
            if [[ -n "$file" && -f "$WEBDASH_ROOT/$file" ]]; then
                local dest_dir
                dest_dir=$(dirname "$test_dir/webdash/$file")
                mkdir -p "$dest_dir"
                cp "$WEBDASH_ROOT/$file" "$test_dir/webdash/$file"
            fi
        done <<< "$untracked_files"
        log_success "Untracked files copied"
    fi

    # Run setup in test mode
    log_info "Running setup in sandboxed environment..."
    echo ""
    echo "=============================================="
    echo "  ENTERING SANDBOXED SETUP"
    echo "  WEBDASH=$test_dir/webdash"
    echo "=============================================="
    echo ""

    # Export test mode and run the setup script in the sandbox
    export WEBDASH_TEST_MODE="true"
    cd "$test_dir/webdash"
    bash "./install.sh"

    local exit_code=$?

    echo ""
    echo "=============================================="
    if [[ $exit_code -eq 0 ]]; then
        log_success "SANDBOXED TEST COMPLETED SUCCESSFULLY"
    else
        log_error "SANDBOXED TEST FAILED (exit code: $exit_code)"
    fi
    echo "=============================================="
    echo ""
    log_info "Test directory preserved at: $test_dir/webdash"
    log_info "To clean up: rm -rf $test_dir"

    exit $exit_code
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --test-setup)
                run_test_setup
                exit 0
                ;;
            --help|-h)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  --test-setup    Run setup in a sandboxed test environment"
                echo "  --help, -h      Show this help message"
                echo ""
                echo "Environment variables:"
                echo "  WEBDASH_TEST_MODE=true    Skip system modifications (bashrc, systemd)"
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                echo "Use --help for usage information"
                exit 1
                ;;
        esac
        shift
    done
}

parse_args "$@"
main
