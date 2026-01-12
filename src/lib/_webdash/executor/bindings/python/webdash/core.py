"""
WebDash core functionality - root detection and path management.

This module implements the same algorithms as the C++ webdash-executor library,
ensuring consistent behavior across all language bindings.
"""

import json
import os
from pathlib import Path
from typing import Optional, Any
from functools import lru_cache


# Constants matching webdash-constants.hpp
PROFILE_FILENAME = "webdash-profile.json"
CONFIG_FILENAME = "webdash.config.json"
PERSISTENT_STORAGE_DIR = "app-persistent"
TEMPORARY_STORAGE_DIR = "app-temporary"
DATA_SUBDIR = "data"
LOGGING_SUBDIR = "logging"
ENV_VAR_NAME = "WEBDASH"

# Magic key/value for root validation (from WebDashConst::Internal)
MAGIC_PROFILE_KEY = "webdash.rootDir"
MAGIC_PROFILE_VALUE = "this"


# Module-level project name (set by consuming code)
_project_name: Optional[str] = None


def set_project_name(name: str) -> None:
    """
    Set the project name for this module.

    This is equivalent to defining _WEBDASH_PROJECT_NAME_ in C++.
    Must be called before using storage APIs.

    Args:
        name: The project name (e.g., "android-app-sync")
    """
    global _project_name
    _project_name = name
    # Clear cached paths since they depend on project name
    get_persistent_storage_path.cache_clear()
    get_temporary_storage_path.cache_clear()


def get_project_name() -> str:
    """
    Get the current project name.

    Returns:
        The project name set via set_project_name()

    Raises:
        RuntimeError: If project name hasn't been set
    """
    if _project_name is None:
        raise RuntimeError(
            "Project name not set. Call webdash.set_project_name('your-project') first."
        )
    return _project_name


@lru_cache(maxsize=1)
def get_root_directory() -> Path:
    """
    Find the WebDash root directory.

    Searches upward from the current directory for a webdash-profile.json
    file containing the magic key-value pair that identifies the root.
    Also checks the WEBDASH environment variable.

    This mirrors the C++ algorithm in WebDashCore::_CalculateRootDirectory().

    Returns:
        Path to the WebDash root directory

    Raises:
        RuntimeError: If no valid WebDash root can be found
    """
    # Check environment variable first
    env_root = os.environ.get(ENV_VAR_NAME)
    if env_root:
        env_path = Path(env_root)
        if _is_valid_webdash_root(env_path):
            return env_path

    # Start from current working directory
    current = Path.cwd().resolve()

    # Traverse upward looking for webdash-profile.json
    while True:
        if _is_valid_webdash_root(current):
            return current

        parent = current.parent
        if parent == current:
            # Reached filesystem root
            break
        current = parent

    raise RuntimeError(
        f"Could not find WebDash root directory.\n"
        f"No {PROFILE_FILENAME} with '{MAGIC_PROFILE_KEY}': '{MAGIC_PROFILE_VALUE}' "
        f"found in directory ancestry.\n"
        f"Set the {ENV_VAR_NAME} environment variable or run from within a WebDash workspace."
    )


def _is_valid_webdash_root(directory: Path) -> bool:
    """
    Check if a directory contains a valid webdash-profile.json.

    Args:
        directory: Path to check

    Returns:
        True if directory contains valid WebDash profile
    """
    profile_path = directory / PROFILE_FILENAME
    if not profile_path.exists():
        return False

    try:
        with open(profile_path, 'r') as f:
            data = json.load(f)

        # Check for the magic key: webdash.rootDir = "this"
        webdash_section = data.get("webdash", {})
        return webdash_section.get("rootDir") == MAGIC_PROFILE_VALUE
    except (json.JSONDecodeError, IOError, KeyError):
        return False


@lru_cache(maxsize=1)
def get_persistent_storage_path() -> Path:
    """
    Get the project-specific persistent storage directory.

    This follows the webdash convention:
        <root>/app-persistent/data/<project_name>/

    The directory is created if it doesn't exist.

    Returns:
        Path to persistent storage directory

    Raises:
        RuntimeError: If project name not set or root not found
    """
    root = get_root_directory()
    project = get_project_name()

    storage_path = root / PERSISTENT_STORAGE_DIR / DATA_SUBDIR / project
    storage_path.mkdir(parents=True, exist_ok=True)

    return storage_path


@lru_cache(maxsize=1)
def get_temporary_storage_path() -> Path:
    """
    Get the project-specific temporary storage directory.

    This follows the webdash convention:
        <root>/app-temporary/<project_name>/

    The directory is created if it doesn't exist.

    Returns:
        Path to temporary storage directory

    Raises:
        RuntimeError: If project name not set or root not found
    """
    root = get_root_directory()
    project = get_project_name()

    storage_path = root / TEMPORARY_STORAGE_DIR / project
    storage_path.mkdir(parents=True, exist_ok=True)

    return storage_path


def get_profile_value(key_path: str) -> Optional[str]:
    """
    Get a value from webdash-profile.json by dot-separated key path.

    Args:
        key_path: Dot-separated path like "ports.my-service" or "env.WEBDASH"

    Returns:
        The value as a string, or None if not found

    Example:
        >>> get_profile_value('ports.src-static-wallpaper-engine-dashboard-monitor')
        '8765'
    """
    root = get_root_directory()
    profile_path = root / PROFILE_FILENAME

    try:
        with open(profile_path, 'r') as f:
            data = json.load(f)

        # Navigate the nested structure
        keys = key_path.split('.')
        value: Any = data

        for key in keys:
            if isinstance(value, dict):
                value = value.get(key)
            else:
                return None

            if value is None:
                return None

        # Convert to string if not already
        if isinstance(value, str):
            return value
        elif isinstance(value, (int, float, bool)):
            return str(value)
        else:
            return None

    except (json.JSONDecodeError, IOError):
        return None


def get_port(port_key: str, default: int = 0) -> int:
    """
    Get a port number from the profile's "ports" section.

    Args:
        port_key: The port key (e.g., "src-static-wallpaper-engine-dashboard-monitor")
        default: Default port if not found

    Returns:
        The port number
    """
    value = get_profile_value(f"ports.{port_key}")
    if value is not None:
        try:
            return int(value)
        except ValueError:
            pass
    return default
