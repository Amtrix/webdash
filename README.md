# WebDash

**One command runner for all your projects.** Define your commands once in a small
JSON file, then run them from anywhere with `webdash <command>` — dependencies,
working directories, and environment included.

Think of it as a personal `make` that works across every project on your machine,
with optional scheduling and a shared environment profile.

---

## Requirements

- Linux (or WSL2 on Windows) — see [Platform support](docs/platform-support.md)
- `git`, `cmake`, `make`, `g++` (>= 9)
- `libboost-all-dev`
- An SSH key configured for GitHub (setup clones over `git@github.com:` URLs)
- Optional: `curl` and `unzip` (the default profile downloads the Android SDK) and `iptables`
  (WSL port forwarding via `webdash --tools.ports.forwarded.add`). Setup warns if they are missing.

On Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install git cmake make g++ libboost-all-dev curl unzip iptables
```

If setup reports that some projects failed to initialize, fix the cause (e.g. install the missing
tool) and run `webdash --profile.reapply`. It is safe to re-run: existing projects are skipped.

## Install

```bash
./install.sh
```

The script builds WebDash, installs the `webdash` binary, and adds an init line to
your `~/.bashrc` (or `~/.zshrc`). Open a new terminal — or `source ~/.bashrc` — and
`webdash` is ready.

> Want to try setup without touching your system? See
> [Testing → Sandboxed setup test](docs/testing.md#sandboxed-setup-test).

## Quick start

Create a `webdash.config.jsonc` in any project:

```json
{
    "commands": [
        { "name": "build", "action": "make", "wdir": "$.thisDir()" },
        { "name": "test",  "action": "make test", "wdir": "$.thisDir()" },
        { "name": "all",   "dependencies": [":build", ":test"] }
    ]
}
```

Then from that directory:

```bash
webdash build   # runs `make`
webdash all     # runs build, then test
```

That's the core loop:

- `:build` runs another command in the same config first.
- `$.thisDir()` and `$.rootDir()` expand to real paths before commands run.
- You can run any project's command by path, no setup needed:
  `webdash /path/to/project:build`.

From here, the [documentation](docs/) goes deep on each topic.

## Documentation

| Guide | What it covers |
| ----- | -------------- |
| [Configuration](docs/configuration.md) | Everything in a project's `webdash.config.jsonc` — commands and every field |
| [Profile](docs/profile.md) | The root `webdash.profile.jsonc` — environment, ports, modules, dependencies, backups |
| [Substitutions & conditions](docs/substitutions.md) | The `$....()` variables and the `condition` mini-language |
| [CLI reference](docs/cli.md) | Every `webdash` command and flag |
| [Scheduling & the server](docs/scheduling.md) | Registration, the background server, and scheduled tasks |
| [Logging](docs/logging.md) | Log files, rotation, and the C++/Node/Python/TypeScript logging APIs |
| [Testing](docs/testing.md) | Running the functional test suite and the sandboxed setup test |
| [Platform support](docs/platform-support.md) | Linux, WSL2, and what's not supported yet |

New to the project? The [documentation index](docs/README.md) suggests a reading
order.

## How it works

```
webdash.profile.jsonc     # one per machine — your environment & project list
        │
        ▼
  webdash (CLI) ──reads──► webdash.config.jsonc   # one per project — commands
        │
        ├──► runs your shell commands (with dependencies, in order)
        │
        └──► webdash-server (optional) ──► runs scheduled commands on its own
```

1. The CLI reads the project config and the root profile.
2. Path variables are substituted with real paths.
3. Dependencies are resolved and executed in order.
4. If running, the [server](docs/scheduling.md) independently handles scheduled
   tasks.

The configs are just shell commands underneath — you can always run them by hand,
so there's no lock-in.

## Project layout

```
webdash.profile.jsonc     # Your environment & projects (machine-specific)
webdash.config.jsonc      # Commands for the root project
install.sh               # Entry point — calls .webdash-setup/setup-webdash.sh
.webdash-setup/          # Setup / bootstrap script
docs/                    # This documentation
src/
    bin/_webdash/        # Core executables (client, server, heartbeat, tests)
    lib/_webdash/        # Core C++ executor library
    lib/external/        # Third-party dependencies (cloned during setup)
    static/              # Standalone web/static projects
app-persistent/          # Installed binaries, libraries, and persistent data
app-temporary/           # Logs, caches, and temporary files (safe to delete)
```

Logs are grouped by project under `app-temporary/logging/<project>/` — see
[Logging](docs/logging.md).
