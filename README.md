# WebDash

**Run project commands from anywhere. Build with dependencies. No more remembering paths.**

WebDash is a command runner that lets you define, share, and execute project commands through simple JSON configs. Navigate to any registered project directory and run `webdash build` — it handles dependencies, sets up the environment, and runs your commands.

> **Think of it as:** A personal `make` that works across all your projects, with built-in scheduling and environment management.

### At a Glance

| What | How |
|------|-----|
| Define commands | JSON config in each project |
| Run commands | `webdash build` from anywhere |
| Chain dependencies | `"dependencies": [":lib-build", "../util:compile"]` |
| Manage environment | One profile sets PATH, env vars, git repos |
| Schedule tasks | `"frequency": "daily"` runs automatically |
| Discover projects | `webdash --list-config` shows everything |

---

## Quick Start

### 1. Install Dependencies

```bash
sudo apt-get update
sudo apt-get install git cmake make libboost-all-dev g++
```

### 2. Run Setup

```bash
./data/setup-webdash.sh
```

That's it. The setup script builds WebDash and configures your shell automatically.

### 3. Start Using WebDash

```bash
# In any project with a webdash.config.json (if build/install/all defined):
webdash build          # Run the "build" command
webdash install        # Run the "install" command
webdash all            # Run everything

# See what's available:
webdash --help         # Show help
webdash --list-config  # List all registered projects
```

---

## Core Concept: Project Commands

Every project can have a `webdash.config.json` that defines commands:

```json
{
    "commands": [
        {
            "name": "build",
            "actions": [
                "mkdir -p build/",
                "cmake -B build",
                "make -C build"
            ],
            "wdir": "$.thisDir()"
        },
        {
            "name": "install",
            "action": "cp build/myapp /usr/local/bin/",
            "wdir": "$.thisDir()"
        },
        {
            "name": "all",
            "dependencies": [":build", ":install"]
        }
    ]
}
```

Then from that directory:
```bash
webdash build    # Runs cmake and make
webdash all      # Runs build, then install
```

### Key Features

| Feature | Example | What it does |
|---------|---------|--------------|
| **Multiple actions** | `"actions": ["cmd1", "cmd2"]` | Run commands in sequence |
| **Dependencies** | `"dependencies": [":build"]` | Run other commands first |
| **Cross-project deps** | `"dependencies": ["$.rootDir()/lib/foo:build"]` | Build another project first |
| **Working directory** | `"wdir": "$.thisDir()/build"` | Run from a specific directory |

### Path Variables

- `$.thisDir()` — Directory containing the current `webdash.config.json`
- `$.rootDir()` — The WebDash root directory (where `webdash-profile.json` lives)

---

## Running Commands from Other Projects

You can run commands from any project by specifying its path directly — **no registration required**:

```bash
webdash /path/to/my/project:build
```

This reads `webdash.config.json` from that path and runs the `build` command.

## Scheduled Execution (Requires Registration)

**This is the main purpose of registration.** When you register a project, the WebDash server can automatically run commands on a schedule.

### How It Works

1. **Register your project** so the server knows about it:
   ```bash
   cd /path/to/my/project
   webdash --register
   ```

2. **Add scheduling fields** to commands in `webdash.config.json`:
   ```json
   {
       "name": "daily-backup",
       "action": "bash backup.sh",
       "frequency": "daily",
       "when": "new-day",
       "wdir": "$.thisDir()"
   }
   ```

3. **The WebDash server** runs in the background and executes scheduled commands automatically.

### Scheduling Options

| Field | Values | Description |
|-------|--------|-------------|
| `frequency` | `"daily"` | Run at most once per 24 hours |
| `frequency` | `"5000"` | Run at most once per 5000 milliseconds |
| `when` | `"new-day"` | Only trigger when the calendar day changes |

Commands with `frequency` set are **only** executed by the server's scheduler — they won't run when you manually call `webdash <command>`.

### Example: Daily Build Report

```json
{
    "name": "report-build-state",
    "actions": ["report-build-state build"],
    "frequency": "daily",
    "when": "new-day",
    "wdir": "$.thisDir()"
}
```

This command runs once per day (on the first check after midnight) and reports the build status.

### Other Registration Benefits

- See all registered projects: `webdash --list-config`
- Reload configs after changes: `webdash --reload-all`

---

## CLI Reference

| Command | Description |
|---------|-------------|
| `webdash <command>` | Run command from `./webdash.config.json` |
| `webdash /path:command` | Run command from a specific project |
| `webdash --help` or `-h` | Show help |
| `webdash --list-config` | List all registered projects |
| `webdash --register` | Register current directory's project |
| `webdash --unregister` | Unregister current directory's project |
| `webdash --reload-all` | Reload all project configurations |
| `webdash --ping-server` | Check if the WebDash server is running |

---

## Example Workflow

```bash
# 1. Create a project config
cat > webdash.config.json << 'EOF'
{
    "commands": [
        {"name": "build", "action": "make", "wdir": "$.thisDir()"},
        {"name": "test", "action": "make test", "wdir": "$.thisDir()"},
        {"name": "all", "dependencies": [":build", ":test"]}
    ]
}
EOF

# 2. Register it
webdash --register

# 3. Use it
webdash build
webdash all
```

---

---

# Appendix

## Directory Structure

WebDash uses a standard directory layout:

```
app-persistent/          # Installed binaries and libraries
    bin/
    lib/
app-temporary/           # Logs and temporary files
    logging/
data/                    # Setup scripts
src/                     # Source code
    bin/                 # Executables
    lib/                 # Libraries
webdash-profile.json     # Global configuration
webdash.terminal.init.sh # Shell initialization
```

## Global Configuration: webdash-profile.json

The root `webdash-profile.json` defines global settings:

```json
{
    "webdash": {
        "rootDir": "this"
    },
    "env": {
        "WEBDASH": "$.rootDir()"
    },
    "path-add": [
        "$.rootDir()/app-persistent/bin"
    ],
    "modules": [
        {
            "path": "$.rootDir()/src/bin/my-tool",
            "git": "git@github.com:user/repo.git",
            "branch": "main",
            "run": ":all",
            "register": true
        }
    ]
}
```

| Field | Purpose |
|-------|---------|
| `webdash.rootDir` | Marks this as the WebDash root (`"this"`) |
| `env` | Environment variables to set |
| `path-add` | Directories to add to `$PATH` |
| `modules` | External git repositories to manage |

### modules options

| Field | Required | Purpose |
|-------|----------|---------|
| `path` | Yes | Where the module lives (target directory) |
| `git` | Yes | Git repository URL |
| `branch` | No | Git branch to checkout (default: repo's default) |
| `run` | No | Command to run after cloning (e.g., `:build`) |
| `register` | No | Auto-register the project (`true`/`false`) |

**Auto-clone behavior:** When a task depends on a path that doesn't exist, WebDash will automatically search `modules` for a matching path and clone it. This enables seamless dependency resolution without manual setup.



## Advanced Command Options

```json
{
    "name": "my-command",
    "actions": ["cmd1", "cmd2"],
    "dependencies": [":other-cmd", "$.rootDir()/lib/foo:build"],
    "wdir": "$.thisDir()/subdir",
    "frequency": "daily",
    "when": "new-day"
}
```

| Field | Purpose |
|-------|---------|
| `name` | Command name (used with `webdash <name>`) |
| `action` / `actions` | Shell command(s) to run |
| `dependencies` | Commands to run first (`:name` for local, full path for external) |
| `wdir` | Working directory |
| `frequency` | How often to run (for scheduled tasks) |
| `when` | Trigger condition |

### Inline Module Dependencies

Dependencies can also include inline module definitions, allowing you to declare git sources directly without requiring a profile entry:

```json
{
    "name": "build",
    "dependencies": [
        ":setup",
        "$.rootDir()/src/lib/core:build",
        {
            "path": "$.rootDir()/src/lib/external-dep",
            "git": "git@github.com:user/external-dep.git",
            "branch": "v2.0",
            "run": ":build"
        }
    ],
    "action": "make"
}
```

When WebDash encounters an inline module dependency whose path doesn't exist, it will automatically clone from the specified git URL before running the task.

## Environment Variable

WebDash sets the `WEBDASH` environment variable to the root directory. This is used internally and can be referenced in your own scripts.

---

## Why WebDash?

### The Problem

Modern development involves juggling multiple projects across different ecosystems:

- **Each project has its own build system** — CMake, Make, npm, cargo, gradle, poetry...
- **Environment setup is manual** — Different PATH entries, env vars, tool versions
- **No unified interface** — You need to remember `npm run build` vs `make` vs `cmake --build`
- **Cross-project dependencies are painful** — Building lib A before app B requires scripts or manual steps
- **Scheduled maintenance is fragmented** — Cron jobs scattered across machines and configs

### The Solution

WebDash provides a **single, consistent interface** for all projects:

```bash
webdash build    # Works whether it's C++, Node.js, Rust, or Python underneath
```

**Key differentiators:**

| Feature | Traditional | WebDash |
|---------|-------------|---------|
| Build command | Different per ecosystem | Always `webdash <task>` |
| Environment | Manual PATH/env setup | Auto-loaded from profile |
| Dependencies | Shell scripts or CI-only | `"dependencies": [":build"]` |
| Cross-project | Complex Makefiles | `"dependencies": ["../lib:build"]` |
| Discovery | Remember all paths | `webdash --list-config` |
| Scheduling | Cron + scattered configs | Inline `"frequency": "daily"` |

### Design Philosophy

1. **JSON configs are self-documenting** — Anyone can read `webdash.config.json` and understand what commands do
2. **No lock-in** — The configs just run shell commands; you can always run them manually
3. **Single source of truth** — One profile defines your entire dev environment
4. **Works alongside existing tools** — WebDash wraps your build system, it doesn't replace it

---

## Future Directions

WebDash is designed to grow. Here are potential extensions:

### Near-term Enhancements

- **Parallel task execution** — Run independent dependencies concurrently
- **Task output caching** — Skip unchanged tasks (like Turborepo/Nx)
- **Better error reporting** — Structured error output with suggestions
- **Config validation** — Warn about common mistakes in `webdash.config.json`

### Ecosystem Integration

- **CI/CD templates** — GitHub Actions / GitLab CI configs that use WebDash
- **IDE plugins** — VS Code extension to run WebDash tasks from the editor
- **Container support** — Run tasks in Docker containers with consistent environments

### Collaboration Features

- **Shared profiles** — Team-wide `webdash-profile.json` with personal overrides
- **Remote execution** — Run tasks on build servers via the WebDash server
- **Dashboard UI** — Web interface showing registered projects, logs, schedules

### Plugin Architecture

The `report-build-state` tool demonstrates the plugin pattern:
- It's a separate binary that uses the WebDash logging infrastructure
- Future plugins could add: test runners, deployment tools, notification systems

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        User's Shell                              │
│  $ webdash build                                                 │
└──────────────────────────────┬──────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                      webdash (CLI client)                        │
│  • Parses webdash.config.json                                    │
│  • Resolves $.thisDir(), $.rootDir() variables                   │
│  • Handles dependencies between tasks                            │
│  • Communicates with server for registration                     │
└──────────────────────────────┬──────────────────────────────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
              ▼                ▼                ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│ libwebdash-     │  │ webdash-server  │  │ webdash-profile │
│ executor        │  │ (systemd)       │  │ .json           │
│                 │  │                 │  │                 │
│ • Config parsing│  │ • Registration  │  │ • Environment   │
│ • Path resolving│  │ • Scheduling    │  │ • PATH entries  │
│ • Logging API   │  │ • Background    │  │ • Modules       │
│ • Substitutions │  │   task runner   │  │ • Custom vars   │
└─────────────────┘  └─────────────────┘  └─────────────────┘
```

**Data flow:**
1. CLI reads `webdash.config.json` and `webdash-profile.json`
2. Variables like `$.thisDir()` are substituted with actual paths
3. Dependencies are resolved and executed in order
4. Server (if running) handles scheduled tasks independently
