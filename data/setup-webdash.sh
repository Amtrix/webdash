#!/bin/bash
#
# WebDash Setup Script
# ====================
#
# PURPOSE:
#   Bootstrap a fresh WebDash development environment by:
#   1. Cloning required dependencies (external libraries, server, etc.)
#   2. Building the core WebDash library (webdash-executor)
#   3. Building and installing the WebDash CLI client
#   4. Setting up the WebDash server as a systemd service
#   5. Generating shell initialization scripts
#
# PREREQUISITES:
#   - git, cmake, make, g++ (>= 9), libboost-all-dev
#   - SSH keys configured for GitHub (uses git@github.com URLs)
#
# USAGE:
#   ./data/setup-webdash.sh              # Normal setup
#   ./data/setup-webdash.sh --test-setup # Sandboxed test run
#
# AFTER RUNNING:
#   Add to your ~/.bashrc:
#     export WEBDASH="/path/to/webdash"
#     source "$WEBDASH/webdash.terminal.init.sh"
#

set -euo pipefail  # Exit on error, undefined vars, and pipe failures

# =============================================================================
# CONFIGURATION
# =============================================================================

# Test mode flag - when true, skip system modifications
WEBDASH_TEST_MODE="${WEBDASH_TEST_MODE:-false}"

# Resolve script location and WebDash root directory.
# Why: We need absolute paths that work regardless of where the script is called from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
WEBDASH_ROOT="$(realpath "$SCRIPT_DIR/..")"

# C++ header that defines canonical constants (single source of truth).
CPP_CONSTANTS_HEADER="$WEBDASH_ROOT/src/lib/webdash-executor/include/webdash-constants.hpp"

# Extract canonical filenames from C++ header to avoid duplication.
# Why: The C++ library is the source of truth for these constants.
#      By reading from the header, we ensure bash and C++ stay in sync.
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

# Export for child processes and builds.
# Why: CMake and other build tools read WEBDASH to locate dependencies.
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

# Build a CMake project
# Why: Centralizes build logic and error handling in one place.
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

# Clone bootstrap dependencies by parsing webdash-profile.json directly.
# These must be cloned BEFORE webdash can be built (chicken-and-egg problem).
# Uses jq if available, otherwise falls back to grep/sed parsing.

clone_bootstrap_dependencies() {
    local profile="$WEBDASH/$PROFILE_FILENAME"

    if [[ ! -f "$profile" ]]; then
        log_warn "Profile not found, copying default..."
        cp "$WEBDASH/data/webdash-profile.default.json" "$profile"
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
    done < "$profile"
}

clone_single_repo() {
    local dest_path="$1"
    local git_url="$2"
    local branch="$3"

    if [[ -d "$dest_path" ]]; then
        log_info "Already exists: $dest_path (skipping)"
        return 0
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
        PROFILE_FILENAME="webdash-profile.json"
        TERMINAL_INIT_FILENAME="webdash.terminal.init.sh"
    else
        PROFILE_FILENAME=$(extract_cpp_constant "kRootProfileFilename")
        TERMINAL_INIT_FILENAME=$(extract_cpp_constant "kTerminalInitFilename")
        log_info "Loaded constants from C++ header:"
        log_info "  PROFILE_FILENAME=$PROFILE_FILENAME"
        log_info "  TERMINAL_INIT_FILENAME=$TERMINAL_INIT_FILENAME"
    fi

    log_warn "This setup may erase and re-clone directories listed above."
    log_info "WebDash root: $WEBDASH"
    echo ""

    # -------------------------------------------------------------------------
    # Step 0: Verify prerequisites
    # -------------------------------------------------------------------------
    log_info "Checking prerequisites..."
    require_command git
    require_command cmake
    require_command make
    require_command g++

    # -------------------------------------------------------------------------
    # Step 1: Create directory structure
    # Why: WebDash enforces a specific layout for consistency across projects
    # -------------------------------------------------------------------------
    log_info "Creating directory structure..."

    mkdir -p "$WEBDASH/app-temporary"        # Logs, caches, temp files (gitignored)
    mkdir -p "$WEBDASH/app-persistent/bin"   # Compiled binaries
    mkdir -p "$WEBDASH/app-persistent/lib"   # Compiled libraries
    mkdir -p "$WEBDASH/app-persistent/data"  # Persistent app data

    # -------------------------------------------------------------------------
    # Step 2: Clone bootstrap dependencies
    # Why: External libraries (json, websocketpp) must be cloned before
    #      we can build webdash-executor. After webdash is built, it can
    #      auto-clone modules defined in the profile.
    # -------------------------------------------------------------------------
    clone_bootstrap_dependencies

    # -------------------------------------------------------------------------
    # Step 3: Build WebDash Executor Library
    # Why: This is the core library that both client and server depend on.
    #      It provides config parsing, path resolution, logging, etc.
    #      MUST be built before the client.
    # -------------------------------------------------------------------------
    build_cmake_project "$WEBDASH/src/lib/webdash-executor" "WebDash Executor Library"

    # -------------------------------------------------------------------------
    # Step 4: Build and Install WebDash Client
    # Why: The 'webdash' CLI binary that users interact with.
    #      Links against the executor library built above.
    # -------------------------------------------------------------------------
    build_cmake_project "$WEBDASH/src/bin/_webdash-client" "WebDash Client"

    log_info "Installing WebDash client to app-persistent/bin..."
    pushd "$WEBDASH/src/bin/_webdash-client" > /dev/null
    chmod +x install.sh
    ./install.sh
    popd > /dev/null
    log_success "WebDash client installed"

    # -------------------------------------------------------------------------
    # Step 5: Initialize profile configuration
    # Why: The $PROFILE_FILENAME defines environment, paths, and projects.
    #      We copy a default if none exists (-n = no clobber).
    # -------------------------------------------------------------------------
    if [[ ! -f "$WEBDASH/$PROFILE_FILENAME" ]]; then
        log_info "Creating $PROFILE_FILENAME from default..."
        cp "$WEBDASH/data/webdash-profile.default.json" "$WEBDASH/$PROFILE_FILENAME"
    else
        log_info "$PROFILE_FILENAME already exists, keeping it"
    fi

    # -------------------------------------------------------------------------
    # Step 6: Generate shell initialization script
    # Why: Users source this in their shell to get:
    #      - WEBDASH environment variable
    #      - PATH updated with WebDash binaries
    #      - Any custom env vars from $PROFILE_FILENAME
    #
    # This script calls the webdash client to regenerate the actual init file,
    # ensuring it stays up-to-date with profile changes.
    # -------------------------------------------------------------------------
    log_info "Generating shell initialization script..."

    cat > "$WEBDASH/$TERMINAL_INIT_FILENAME" << EOF
# WebDash Shell Initialization
# ============================
# Source this file in your ~/.bashrc or ~/.zshrc:
#   source "/path/to/webdash/$TERMINAL_INIT_FILENAME"
#
# This script:
# 1. Regenerates environment settings from $PROFILE_FILENAME
# 2. Sources the generated init file with PATH and env vars
#
# Auto-generated by setup-webdash.sh - modifications will be overwritten.

# Regenerate init file from current profile (picks up any changes)
"$WEBDASH/app-persistent/bin/webdash" _internal_:create-build-init 2>/dev/null

# Source the generated environment
if [[ -f "$WEBDASH/app-persistent/data/webdash-client/$TERMINAL_INIT_FILENAME" ]]; then
    source "$WEBDASH/app-persistent/data/webdash-client/$TERMINAL_INIT_FILENAME"
fi
EOF

    log_success "Shell init script created: \$WEBDASH/$TERMINAL_INIT_FILENAME"

    # -------------------------------------------------------------------------
    # Step 7: Build and start WebDash Server
    # Why: The server runs as a background service (systemd) and:
    #      - Tracks all registered webdash.config.json files
    #      - Can run scheduled tasks (daily builds, etc.)
    #      - Provides a central point for config discovery
    # -------------------------------------------------------------------------
    if [[ "$WEBDASH_TEST_MODE" == "true" ]]; then
        log_info "[TEST MODE] Building WebDash Server (skipping install/systemd)..."
        if ! "$WEBDASH/app-persistent/bin/webdash" "$WEBDASH/src/bin/_webdash-server:build"; then
            log_error "Failed to build WebDash server"
            show_recent_errors
            exit 1
        fi
        # Verify the binary was actually created
        if [[ ! -f "$WEBDASH/src/bin/_webdash-server/build/webdash-server" ]]; then
            log_error "WebDash server build failed - binary not found"
            show_recent_errors
            exit 1
        fi
        log_success "WebDash server built (not installed in test mode)"
    else
        log_info "Building and starting WebDash Server..."
        if ! "$WEBDASH/app-persistent/bin/webdash" "$WEBDASH/src/bin/_webdash-server:all"; then
            log_error "Failed to build/start WebDash server"
            log_warn "You can try manually later: webdash \$WEBDASH/src/bin/_webdash-server:all"
        else
            log_success "WebDash server started"
        fi
    fi

    # -------------------------------------------------------------------------
    # Step 8: Clone and register projects from profile
    # Why: $PROFILE_FILENAME can specify external projects to clone.
    #      This step fetches them and registers their configs with the server.
    # -------------------------------------------------------------------------
    log_info "Processing projects from $PROFILE_FILENAME..."

    # Generate the clone script from profile
    "$WEBDASH/app-persistent/bin/webdash" _internal_:create-project-cloner 2>/dev/null || true

    local init_script="$WEBDASH/app-persistent/data/webdash-client/initialize-projects.sh"
    if [[ -f "$init_script" ]]; then
        chmod +x "$init_script"
        "$init_script" || log_warn "Some projects may have failed to initialize"
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
    local source_line="source \"\$WEBDASH/$TERMINAL_INIT_FILENAME\""
    local marker="# WebDash initialization"

    # Check if already configured (idempotent - won't add twice)
    if grep -qF "$export_line" "$shell_rc" 2>/dev/null; then
        log_info "WebDash already configured in $shell_rc (skipping)"
    else
        log_info "Adding WebDash initialization to $shell_rc..."
        {
            echo ""
            echo "$marker"
            echo "$export_line"
            echo "$source_line"
        } >> "$shell_rc"
        log_success "Added WebDash initialization to $shell_rc"
    fi

    # Source the shell config to activate WebDash in current session
    log_info "Activating WebDash in current shell..."
    export WEBDASH="$WEBDASH_ROOT"
    source "$WEBDASH/$TERMINAL_INIT_FILENAME"
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

    # Get current branch and remote
    local current_branch
    current_branch=$(git -C "$WEBDASH_ROOT" branch --show-current 2>/dev/null || echo "main")
    local remote_url
    remote_url=$(git -C "$WEBDASH_ROOT" remote get-url origin 2>/dev/null || echo "")

    if [[ -z "$remote_url" ]]; then
        log_error "Could not determine git remote URL"
        exit 1
    fi

    log_info "Current branch: $current_branch"
    log_info "Remote: $remote_url"

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

    # Clone the repo fresh
    log_info "Cloning $current_branch into test directory..."
    if ! git clone --branch "$current_branch" --single-branch "$remote_url" "$test_dir/webdash" 2>&1; then
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
    bash "./data/setup-webdash.sh"

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
