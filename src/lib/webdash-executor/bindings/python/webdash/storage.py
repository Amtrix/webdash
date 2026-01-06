"""
WebDash storage API - persistent and temporary file storage.

Provides a high-level interface for reading and writing files to
project-specific storage locations, mirroring the C++ Storage API.
"""

import json
from pathlib import Path
from typing import Optional, Any, Union, IO
from contextlib import contextmanager

from .core import get_persistent_storage_path, get_temporary_storage_path


class Storage:
    """
    High-level storage interface for WebDash projects.

    Provides convenient methods for reading/writing files to the
    project's persistent or temporary storage directories.

    Example:
        storage = Storage()

        # Write JSON data
        storage.write_json('config.json', {'key': 'value'})

        # Read JSON data
        data = storage.read_json('config.json')

        # Write text
        storage.write_text('log.txt', 'Hello world')

        # Append to file
        storage.append_text('log.txt', 'More text')
    """

    def __init__(self, temporary: bool = False):
        """
        Initialize storage interface.

        Args:
            temporary: If True, use temporary storage instead of persistent
        """
        self._temporary = temporary

    @property
    def base_path(self) -> Path:
        """Get the base storage path."""
        if self._temporary:
            return get_temporary_storage_path()
        return get_persistent_storage_path()

    def get_path(self, filename: str) -> Path:
        """
        Get the full path for a file in storage.

        Args:
            filename: Relative filename within storage

        Returns:
            Full path to the file
        """
        return self.base_path / filename

    def exists(self, filename: str) -> bool:
        """Check if a file exists in storage."""
        return self.get_path(filename).exists()

    def read_text(self, filename: str, default: str = "") -> str:
        """
        Read text from a file in storage.

        Args:
            filename: File to read
            default: Default value if file doesn't exist

        Returns:
            File contents or default
        """
        path = self.get_path(filename)
        try:
            return path.read_text()
        except (IOError, FileNotFoundError):
            return default

    def write_text(self, filename: str, content: str) -> None:
        """
        Write text to a file in storage.

        Args:
            filename: File to write
            content: Content to write
        """
        path = self.get_path(filename)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def append_text(self, filename: str, content: str) -> None:
        """
        Append text to a file in storage.

        Args:
            filename: File to append to
            content: Content to append
        """
        path = self.get_path(filename)
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, 'a') as f:
            f.write(content)

    def read_json(self, filename: str, default: Optional[Any] = None) -> Any:
        """
        Read JSON from a file in storage.

        Args:
            filename: File to read
            default: Default value if file doesn't exist or is invalid JSON

        Returns:
            Parsed JSON data or default (defaults to empty dict if not specified)
        """
        if default is None:
            default = {}

        path = self.get_path(filename)
        try:
            with open(path, 'r') as f:
                return json.load(f)
        except (IOError, FileNotFoundError, json.JSONDecodeError):
            return default

    def write_json(self, filename: str, data: Any, indent: int = 2) -> None:
        """
        Write JSON to a file in storage.

        Args:
            filename: File to write
            data: Data to serialize as JSON
            indent: JSON indentation (default: 2)
        """
        path = self.get_path(filename)
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, 'w') as f:
            json.dump(data, f, indent=indent)

    def read_bytes(self, filename: str) -> Optional[bytes]:
        """
        Read binary data from a file in storage.

        Args:
            filename: File to read

        Returns:
            File contents or None if not found
        """
        path = self.get_path(filename)
        try:
            return path.read_bytes()
        except (IOError, FileNotFoundError):
            return None

    def write_bytes(self, filename: str, data: bytes) -> None:
        """
        Write binary data to a file in storage.

        Args:
            filename: File to write
            data: Binary data to write
        """
        path = self.get_path(filename)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    @contextmanager
    def open(self, filename: str, mode: str = 'r'):
        """
        Open a file in storage with context manager.

        Args:
            filename: File to open
            mode: File mode ('r', 'w', 'a', 'rb', 'wb', etc.)

        Yields:
            File handle
        """
        path = self.get_path(filename)
        path.parent.mkdir(parents=True, exist_ok=True)

        with open(path, mode) as f:
            yield f

    def delete(self, filename: str) -> bool:
        """
        Delete a file from storage.

        Args:
            filename: File to delete

        Returns:
            True if file was deleted, False if it didn't exist
        """
        path = self.get_path(filename)
        try:
            path.unlink()
            return True
        except FileNotFoundError:
            return False

    def list_files(self, pattern: str = "*") -> list[Path]:
        """
        List files in storage matching a pattern.

        Args:
            pattern: Glob pattern (default: "*" for all files)

        Returns:
            List of matching file paths
        """
        return list(self.base_path.glob(pattern))


# Convenience instances
persistent_storage = Storage(temporary=False)
temporary_storage = Storage(temporary=True)
