# WebDash Python Bindings

Python bindings for the WebDash framework, providing access to:

- **Root directory detection** - Find WebDash root from any subdirectory
- **Persistent storage** - Project-specific data storage that survives updates
- **Temporary storage** - Project-specific temporary files
- **Logging** - Structured logging with automatic rotation
- **Profile access** - Read values from webdash-profile.json

## Installation

### Development install (editable)

```bash
cd src/lib/webdash-executor/bindings/python
pip install -e .
```

### From within a WebDash project

```bash
pip install -e $WEBDASH/src/lib/webdash-executor/bindings/python
```

## Quick Start

```python
import webdash

# Set your project name (required for storage APIs)
webdash.set_project_name('my-project')

# Get WebDash root directory
root = webdash.get_root_directory()
print(f"WebDash root: {root}")

# Get project storage path
storage_path = webdash.get_persistent_storage_path()
print(f"Storage: {storage_path}")
# => <root>/app-persistent/data/my-project/

# Use the Storage API for file operations
storage = webdash.Storage()

# Write/read JSON
storage.write_json('config.json', {'key': 'value'})
config = storage.read_json('config.json')

# Write/read text
storage.write_text('data.txt', 'Hello world')
text = storage.read_text('data.txt')

# Logging
webdash.log.info('Application started')
webdash.log.error('Something went wrong', {'code': 500})
webdash.log.debug('Processing item', {'id': 123})
```

## API Reference

### Core Functions

#### `webdash.set_project_name(name: str)`
Set the project name. Must be called before using storage APIs.

#### `webdash.get_project_name() -> str`
Get the current project name.

#### `webdash.get_root_directory() -> Path`
Find and return the WebDash root directory.

#### `webdash.get_persistent_storage_path() -> Path`
Get the project's persistent storage directory.
Returns: `<root>/app-persistent/data/<project_name>/`

#### `webdash.get_temporary_storage_path() -> Path`
Get the project's temporary storage directory.
Returns: `<root>/app-temporary/<project_name>/`

#### `webdash.get_profile_value(key_path: str) -> Optional[str]`
Get a value from webdash-profile.json by dot-separated key path.

### Storage Class

```python
storage = webdash.Storage(temporary=False)

storage.read_text(filename, default="")
storage.write_text(filename, content)
storage.append_text(filename, content)

storage.read_json(filename, default={})
storage.write_json(filename, data, indent=2)

storage.read_bytes(filename) -> Optional[bytes]
storage.write_bytes(filename, data)

storage.exists(filename) -> bool
storage.delete(filename) -> bool
storage.list_files(pattern="*") -> list[Path]

with storage.open(filename, mode='r') as f:
    ...
```

### Logging

```python
webdash.log.info(message, meta=None)
webdash.log.error(message, meta=None)
webdash.log.warn(message, meta=None)
webdash.log.debug(message, meta=None)
webdash.log.notify(message, meta=None)  # Never rotated
```

Log files are written to `<root>/app-temporary/logging/<project_name>/`:
- `logging.info.txt`
- `logging.error.txt`
- `logging.warn.txt`
- `logging.debug.txt`
- `logging.notify.txt`

Files are automatically rotated when they exceed 10 MB.

## Comparison with C++ and Node.js APIs

| Feature | C++ | Node.js | Python |
|---------|-----|---------|--------|
| Root detection | `WebDash().GetWebDashRootDirectory()` | `webdash.getRootDirectory()` | `webdash.get_root_directory()` |
| Persistent storage | `WebDash().GetPersistenteAppStoragePath()` | N/A (manual) | `webdash.get_persistent_storage_path()` |
| Storage write | `WebDash().WriteToAppStorage()` | N/A | `storage.write_text()` / `write_json()` |
| Storage read | `WebDash().LoadFromAppStorage()` | N/A | `storage.read_text()` / `read_json()` |
| Logging | `WebDash().Log()` | `webdash.log.info()` | `webdash.log.info()` |
| Profile values | `WebDash().GetKeyValuesFromRootProfile()` | `webdash.getProfileValue()` | `webdash.get_profile_value()` |

## Environment Variables

- `WEBDASH` - Path to WebDash root directory (optional, used as fallback)

## Directory Structure

```
<webdash-root>/
├── webdash-profile.json          # Root marker file
├── app-persistent/
│   └── data/
│       └── <project-name>/       # Persistent storage
│           └── your-files.json
└── app-temporary/
    ├── <project-name>/           # Temporary storage
    └── logging/
        └── <project-name>/       # Log files
            ├── logging.info.txt
            ├── logging.error.txt
            └── ...
```
