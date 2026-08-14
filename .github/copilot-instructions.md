# WebDash AI Instructions

## Project Mental Model

WebDash is a personal command runner and scheduler. It uses JSON project configs to define commands, dependencies, working directories, background tasks, and scheduled tasks.

The main contract is `webdash.config.jsonc`:
- Commands live under `commands`.
- Local dependencies use `:command`.
- Cross-project dependencies use `<path>:command`.
- Path substitutions include `$.thisDir()`, `$.rootDir()`, `$.path()`, `$.webdash.persistent()`, and `$.webdash.temporary()`.
- Scheduled commands use `frequency`, optionally `when` and `condition`.
- Background commands use `"background": true`.

The root profile is `webdash.profile.jsonc`:
- Defines the WebDash root, environment variables, PATH additions, ports, modules, dependencies, backup config, and editor/runtime settings.
- Treat it as user/machine-specific unless the task explicitly asks for profile changes.
- Understand `modules` as WebDash projects with commands and `dependencies` as external libraries or setup dependencies.

## Important Directories

Work primarily in:
- `src/` for source code.
- `src/bin/_webdash/` for core WebDash executables.
- `src/lib/_webdash/executor/` for the core C++ executor library.
- `src/static/` for individual web/static projects.
- `.webdash-setup/` for setup/bootstrap logic.
- Root and nested `webdash.config.jsonc` files for task definitions.

Avoid editing unless explicitly needed:
- `app-persistent/`, because it contains installed binaries, runtime data, copied static assets, and generated files.
- `app-temporary/`, because it contains logs, caches, and temporary test/setup artifacts.
- `src/lib/external/`, because it contains external dependencies.
- `node_modules/`, build folders, generated logs, and copied runtime web UI output.

## Common Commands

Prefer the project's WebDash interface when possible:
- `webdash --help`
- `webdash --list-registered-configs`
- `webdash --ping-server`
- `webdash <command>`
- `webdash /path/to/project:command`

For setup validation:
- `./install.sh --test-setup`

For normal setup:
- `./install.sh`

Use the sandboxed setup test before changing bootstrap, profile dependency behavior, install logic, shell init generation, or systemd/server setup.

## Build And Test Guidance

The core implementation is mixed-language:
- C++20 for the executor, CLI, and server.
- Bash for setup/bootstrap.
- Node.js and Python bindings/utilities.
- Static web projects under `src/static/*`.

C++ builds generally use CMake and Make. Before changing C++ code, inspect the local `CMakeLists.txt` and the relevant `webdash.config.jsonc`.

When changing a static project:
- Check that project's own `package.json`.
- Check its local `webdash.config.jsonc`.
- Be aware that some commands copy source files into `app-persistent/` and run from there.

When changing setup behavior:
- Prefer `./install.sh --test-setup`.
- Do not assume systemd or shell profile changes should be made during tests.
- Check `app-temporary/logging/` after failures.

## Logging And Debugging

Logs live under `app-temporary/logging/`, usually grouped by project name.

Common log files:
- `logging.info.txt`
- `logging.warn.txt`
- `logging.error.txt`
- `logging.debug.txt`
- `logging.notify.txt`

When debugging WebDash behavior:
1. Identify the active project config.
2. Resolve whether the command is manual, background, or scheduled.
3. Check dependencies and path substitutions.
4. Check the relevant project logs in `app-temporary/logging/`.
5. Check registered configs with `webdash --list-registered-configs` when scheduler/server behavior is involved.

## Editing Rules

Keep changes scoped to source/config files. Do not patch generated runtime files to fix source behavior.

When editing `webdash.config.jsonc`:
- Preserve valid JSON.
- Prefer `actions` for ordered multi-step command sequences.
- Use `wdir` explicitly when command behavior depends on location.
- Prefer WebDash substitutions over hardcoded absolute paths.
- Use `:command` for local dependencies.
- Use `$.rootDir()/...:command` for stable cross-project dependencies.
- Do not add `frequency` to commands that should be manually runnable; scheduled commands are server-runner concepts.

When editing `webdash.profile.jsonc`:
- Be careful with user-specific paths, ports, backup locations, editor settings, and private repository URLs.
- Do not remove existing module/dependency entries unless explicitly asked.
- Preserve the distinction between `modules` and `dependencies`.

When editing C++:
- Check headers in `src/lib/_webdash/executor/include/`.
- Keep constants synchronized with `webdash-constants.hpp` when changing names, directories, env vars, or file conventions.
- Prefer existing executor/config/path-resolution utilities over duplicating parsing logic.

When editing setup scripts:
- Avoid hardcoded values that already exist in WebDash constants/profile/config.
- Preserve test setup behavior.
- Avoid making system changes in test mode.

## Dependency And Module Behavior

WebDash can auto-clone missing module paths based on `webdash.profile.jsonc` or inline module dependencies.

Before assuming a missing path is an error:
- Check `modules` in `webdash.profile.jsonc`.
- Check inline dependency objects in the relevant `webdash.config.jsonc`.
- Check whether `interactive-clone` is enabled or disabled.

Do not vendor new external dependencies into `src/lib/external/` manually unless explicitly requested. Prefer adding dependency metadata to the profile or config in the established style.

## Scheduler And Server Behavior

Registration is mainly for scheduled/server execution.

Manual command execution does not require registration:
- `webdash /path/to/project:command`

Scheduled execution requires:
- The project to be registered.
- A command with `frequency`.
- The WebDash server to be running.

When debugging scheduled tasks:
- Check `webdash --ping-server`.
- Check `webdash --list-registered-configs`.
- Check `app-persistent/data/webdash-server/registered-configs.txt`.
- Check logs under `app-temporary/logging/webdash-server/`.

## Agent Workflow

Before making changes:
1. Read the relevant `webdash.config.jsonc`.
2. Read the relevant source file or setup script.
3. Check whether the target path is source, persistent runtime output, temporary output, or external dependency code.
4. Identify the smallest command that validates the change.

After making changes:
1. Validate JSON syntax for changed config/profile files.
2. Run the narrowest relevant WebDash/build/test command.
3. For setup changes, run `./install.sh --test-setup` when feasible.
4. Summarize changed source files and verification commands.

Do not commit changes unless explicitly asked.
