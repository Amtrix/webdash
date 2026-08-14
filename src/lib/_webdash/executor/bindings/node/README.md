# WebDash Node.js Bindings

N-API bindings that expose WebDash framework functionality to Node.js applications.

## Features

- **Logging** - Use WebDash's unified logging system (logs to `app-temporary/logging/<project>/`)
- **Root Directory** - Access the WebDash root directory path
- **Cross-platform** - Works on Linux and Windows
- **JSONC-aware** - `webdash.config.jsonc`/`webdash.profile.jsonc` may use `//`/`/* */` comments and trailing commas; this binding strips them before parsing, same as the C++ core.

## Installation

```bash
# From this directory
npm install
npm run build

# Or from another project
npm install /path/to/webdash/src/lib/webdash-executor/bindings/node
```

### Prerequisites

- Node.js >= 16
- node-gyp (installed automatically)
- C++ compiler with C++20 support
- Boost libraries (`libboost-filesystem-dev` on Debian/Ubuntu)

## Usage

```javascript
const webdash = require('webdash');

// Logging
webdash.log.info('Server started');
webdash.log.error('Connection failed', { code: 500 });
webdash.log.warn('Deprecated feature used');
webdash.log.debug('Request received', { path: '/api' });
webdash.log.notify('Important event');  // Never rotated

// Get WebDash paths
const root = webdash.getRootDirectory();
console.log(`WebDash root: ${root}`);
```

## API

### `webdash.log`

Logging interface with methods for each log level:

| Method | Description |
|--------|-------------|
| `info(message, [meta])` | Informational messages |
| `error(message, [meta])` | Error messages |
| `warn(message, [meta])` | Warning messages |
| `debug(message, [meta])` | Debug messages |
| `notify(message, [meta])` | Notifications (never rotated) |

All methods accept an optional `meta` object that gets JSON-stringified and appended.

### `webdash.getRootDirectory()`

Returns the WebDash root directory path (where `webdash.profile.jsonc` lives).

### `webdash.getProjectName()`

Returns the current project name used for logging.

### `webdash.getProfileValue(keyPath)`

Get a value from `webdash.profile.jsonc` by dot-separated key path.

```javascript
webdash.getProfileValue('ports.my-service')  // "8765"
webdash.getProfileValue('env.WEBDASH')       // resolved path
webdash.getProfileValue('nonexistent.key')   // undefined
```

### `webdash.getPort(portKey, [defaultPort])`

Convenience method to get a port number from the profile's `ports` section.

```javascript
webdash.getPort('src-static-my-service-monitor')        // 8765
webdash.getPort('nonexistent', 3000)                    // 3000 (default)
```

### `webdash.resolveConfigValue(keyPath, [options])`

Resolve a config value by traversing up the directory tree. This checks `webdash.config.jsonc` files in the current directory and parents, then falls back to `webdash.profile.jsonc`.

**Lookup order:**
1. `webdash.config.jsonc` in startDir
2. `webdash.config.jsonc` in parent directories (up to WebDash root)
3. `webdash.profile.jsonc` at WebDash root

**Options:**
- `startDir` - Directory to start searching from (default: `process.cwd()`)
- `skipLocal` - Skip `webdash.config.jsonc` files, only check profile

```javascript
// From current directory
webdash.resolveConfigValue('ports.my-service')

// Start from a specific directory
webdash.resolveConfigValue('mycommands.build', { startDir: '/path/to/project' })

// Only check webdash.profile.jsonc
webdash.resolveConfigValue('env.WEBDASH', { skipLocal: true })
```

## Building from Source

```bash
# Install dependencies
npm install

# Build native addon
npm run build

# Build with debug symbols
npm run build:debug

# Clean build artifacts
npm run clean
```

## Log File Location

Logs are written to:
```
$WEBDASH/app-temporary/logging/<project-name>/
├── logging.info.txt
├── logging.error.txt
├── logging.warn.txt
├── logging.debug.txt
└── logging.notify.txt
```

Log files are automatically rotated when they exceed 10 MB.
