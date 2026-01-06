"""
WebDash Python bindings

Provides access to WebDash framework functionality from Python:
- Logging (info, error, warn, debug, notify)
- WebDash root directory access
- Persistent storage API
- Config value resolution

Example:
    import webdash

    webdash.log.info('Processing started')
    webdash.log.error('Connection failed')

    root = webdash.get_root_directory()
    storage = webdash.get_persistent_storage_path()
"""

from .core import (
    get_root_directory,
    get_project_name,
    get_persistent_storage_path,
    get_temporary_storage_path,
    get_profile_value,
    set_project_name,
)

from .storage import Storage

from .log import log

__all__ = [
    'get_root_directory',
    'get_project_name',
    'get_persistent_storage_path',
    'get_temporary_storage_path',
    'get_profile_value',
    'set_project_name',
    'Storage',
    'log',
]

__version__ = '0.1.0'
