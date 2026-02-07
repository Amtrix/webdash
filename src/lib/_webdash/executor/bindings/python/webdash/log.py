"""
WebDash logging API - project-specific logging with rotation.

Provides logging functionality that writes to project-specific log files
in the WebDash temporary storage, with automatic file rotation.
"""

import os
import sys
from datetime import datetime
from pathlib import Path
from typing import Optional, Any
from enum import Enum

from .core import get_root_directory, get_project_name, TEMPORARY_STORAGE_DIR, LOGGING_SUBDIR


# Constants matching webdash-constants.hpp
MAX_LOG_FILE_SIZE_BYTES = 10 * 1024 * 1024  # 10 MB
LOG_ROTATION_PREFIX = "previous."
LOG_FILENAME_PREFIX = "logging."


class LogLevel(Enum):
    """Log levels matching WebDashType::LogType."""
    INFO = "info"
    ERROR = "error"
    WARN = "warn"
    DEBUG = "debug"
    NOTIFY = "notify"


def _get_log_directory() -> Path:
    """
    Get the logging directory for the current project.

    Returns:
        Path to: <root>/app-temporary/logging/<project_name>/
    """
    root = get_root_directory()
    project = get_project_name()

    log_dir = root / TEMPORARY_STORAGE_DIR / LOGGING_SUBDIR / project
    log_dir.mkdir(parents=True, exist_ok=True)

    return log_dir


def _get_log_file_path(level: LogLevel) -> Path:
    """Get the path to a specific log file."""
    log_dir = _get_log_directory()
    return log_dir / f"{LOG_FILENAME_PREFIX}{level.value}.txt"


def _rotate_if_needed(log_file: Path) -> None:
    """Rotate log file if it exceeds the size limit."""
    if not log_file.exists():
        return

    try:
        if log_file.stat().st_size > MAX_LOG_FILE_SIZE_BYTES:
            # Rotate: rename current to previous
            previous = log_file.parent / f"{LOG_ROTATION_PREFIX}{log_file.name}"
            if previous.exists():
                previous.unlink()
            log_file.rename(previous)
    except (IOError, OSError):
        pass  # Ignore rotation errors


def _write_log(level: LogLevel, message: str) -> None:
    """
    Write a log message to the appropriate log file.

    Args:
        level: Log level
        message: Message to log
    """
    try:
        log_file = _get_log_file_path(level)
        _rotate_if_needed(log_file)

        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        log_line = f"[{timestamp}] {message}\n"

        with open(log_file, 'a') as f:
            f.write(log_line)
    except Exception:
        # Logging should never crash the application
        pass


def _format_message(message: str, meta: Optional[dict] = None) -> str:
    """Format a log message with optional metadata."""
    if meta:
        import json
        return f"{message} {json.dumps(meta)}"
    return message


class Log:
    """
    Logging interface matching the Node.js webdash.log API.

    Example:
        from webdash import log

        log.info('Server started')
        log.error('Connection failed', {'host': 'localhost'})
        log.debug('Processing request', {'id': 123})
    """

    def info(self, message: str, meta: Optional[dict] = None) -> None:
        """Log an info message."""
        _write_log(LogLevel.INFO, _format_message(message, meta))

    def error(self, message: str, meta: Optional[dict] = None) -> None:
        """Log an error message."""
        _write_log(LogLevel.ERROR, _format_message(message, meta))

    def warn(self, message: str, meta: Optional[dict] = None) -> None:
        """Log a warning message."""
        _write_log(LogLevel.WARN, _format_message(message, meta))

    def debug(self, message: str, meta: Optional[dict] = None) -> None:
        """Log a debug message."""
        _write_log(LogLevel.DEBUG, _format_message(message, meta))

    def notify(self, message: str, meta: Optional[dict] = None) -> None:
        """
        Log a notification (never rotated).

        Notifications are special log messages that are never deleted
        during rotation, making them suitable for important events.
        """
        # For notify, we don't rotate - just append
        try:
            log_file = _get_log_file_path(LogLevel.NOTIFY)
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            log_line = f"[{timestamp}] {_format_message(message, meta)}\n"

            with open(log_file, 'a') as f:
                f.write(log_line)
        except Exception:
            pass


# Singleton instance
log = Log()


class WebDashProject:
    """
    Efficient project-scoped logging with persistent file handles.

    Opens log files once and keeps them open for the lifetime of the project,
    avoiding the overhead of repeated open/close operations.

    Example:
        project = WebDashProject('my-project')
        project.debug('Starting up')
        project.info('Processing...')
        project.end()  # Flushes and closes all files
    """

    def __init__(self, project_name: str):
        """Initialize the project with persistent log file handles."""
        self.project_name = project_name
        self._files = {}
        self._closed = False

        # Get the log directory
        root = get_root_directory()
        self._log_dir = root / TEMPORARY_STORAGE_DIR / LOGGING_SUBDIR / project_name
        self._log_dir.mkdir(parents=True, exist_ok=True)

        # Open all log files
        for level in LogLevel:
            path = self._log_dir / f"{LOG_FILENAME_PREFIX}{level.value}.txt"
            self._rotate_if_needed(path)
            self._files[level] = open(path, 'a')

    def _rotate_if_needed(self, log_file: Path) -> None:
        """Rotate log file if it exceeds the size limit."""
        if not log_file.exists():
            return
        try:
            if log_file.stat().st_size > MAX_LOG_FILE_SIZE_BYTES:
                previous = log_file.parent / f"{LOG_ROTATION_PREFIX}{log_file.name}"
                if previous.exists():
                    previous.unlink()
                log_file.rename(previous)
        except (IOError, OSError):
            pass

    def _log(self, level: LogLevel, message: str) -> None:
        """Write a log message to the appropriate file."""
        if self._closed:
            return
        try:
            f = self._files.get(level)
            if f:
                timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                f.write(f"{timestamp}: {message}\n")
        except Exception:
            pass

    def debug(self, message: str) -> None:
        """Log a debug message."""
        self._log(LogLevel.DEBUG, message)

    def info(self, message: str) -> None:
        """Log an info message."""
        self._log(LogLevel.INFO, message)

    def warn(self, message: str) -> None:
        """Log a warning message."""
        self._log(LogLevel.WARN, message)

    def error(self, message: str) -> None:
        """Log an error message."""
        self._log(LogLevel.ERROR, message)

    def notify(self, message: str) -> None:
        """Log a notification message."""
        self._log(LogLevel.NOTIFY, message)

    def end(self) -> None:
        """Flush and close all log files."""
        if self._closed:
            return
        self._closed = True
        for f in self._files.values():
            try:
                f.flush()
                f.close()
            except Exception:
                pass
        self._files.clear()

    def __del__(self):
        """Ensure files are closed on garbage collection."""
        self.end()


class WebDashProject:
    """
    Efficient project-scoped logging with persistent file handles.

    Opens log files once and keeps them open for the lifetime of the project,
    avoiding the overhead of repeated open/close operations.

    Example:
        project = WebDashProject('my-project')
        project.debug('Starting up')
        project.info('Processing...')
        project.end()  # Flushes and closes all files
    """

    def __init__(self, project_name: str):
        """Initialize the project with persistent log file handles."""
        self.project_name = project_name
        self._files = {}
        self._closed = False

        # Get the log directory
        root = get_root_directory()
        self._log_dir = root / TEMPORARY_STORAGE_DIR / LOGGING_SUBDIR / project_name
        self._log_dir.mkdir(parents=True, exist_ok=True)

        # Open all log files
        for level in LogLevel:
            path = self._log_dir / f"{LOG_FILENAME_PREFIX}{level.value}.txt"
            self._rotate_if_needed(path)
            self._files[level] = open(path, 'a')

    def _rotate_if_needed(self, log_file: Path) -> None:
        """Rotate log file if it exceeds the size limit."""
        if not log_file.exists():
            return
        try:
            if log_file.stat().st_size > MAX_LOG_FILE_SIZE_BYTES:
                previous = log_file.parent / f"{LOG_ROTATION_PREFIX}{log_file.name}"
                if previous.exists():
                    previous.unlink()
                log_file.rename(previous)
        except (IOError, OSError):
            pass

    def _log(self, level: LogLevel, message: str) -> None:
        """Write a log message to the appropriate file."""
        if self._closed:
            return
        try:
            f = self._files.get(level)
            if f:
                timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                f.write(f"{timestamp}: {message}\n")
        except Exception:
            pass

    def debug(self, message: str) -> None:
        """Log a debug message."""
        self._log(LogLevel.DEBUG, message)

    def info(self, message: str) -> None:
        """Log an info message."""
        self._log(LogLevel.INFO, message)

    def warn(self, message: str) -> None:
        """Log a warning message."""
        self._log(LogLevel.WARN, message)

    def error(self, message: str) -> None:
        """Log an error message."""
        self._log(LogLevel.ERROR, message)

    def notify(self, message: str) -> None:
        """Log a notification message."""
        self._log(LogLevel.NOTIFY, message)

    def end(self) -> None:
        """Flush and close all log files."""
        if self._closed:
            return
        self._closed = True
        for f in self._files.values():
            try:
                f.flush()
                f.close()
            except Exception:
                pass
        self._files.clear()

    def __del__(self):
        """Ensure files are closed on garbage collection."""
        self.end()
