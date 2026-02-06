/**
 * WebDashProject - Efficient logging for WebDash projects (Node.js)
 *
 * Keeps log files open for the lifetime of the project, significantly
 * improving performance especially on slow filesystems (like WSL's /mnt/c/).
 *
 * Usage:
 *   const { WebDashProject } = require('webdash/project');
 *
 *   const project = new WebDashProject('my-project');
 *   project.info('Something happened');
 *   project.debug('Debug info');
 *   project.end(); // or let process exit handler close it
 */

'use strict';

const fs = require('fs');
const path = require('path');

// Constants matching webdash-constants.hpp
const MAX_LOG_FILE_SIZE_BYTES = 10 * 1024 * 1024; // 10 MB
const LOG_ROTATION_PREFIX = 'previous.';
const LOG_FILENAME_PREFIX = 'logging.';
const TEMPORARY_STORAGE_DIR = 'app-temporary';
const LOGGING_SUBDIR = 'logging';

const LOG_LEVELS = ['debug', 'info', 'warn', 'error'];

/**
 * Get current timestamp for log entries.
 * @returns {string} Formatted timestamp
 */
function getTimestamp() {
  const now = new Date();
  const pad = (n) => n.toString().padStart(2, '0');
  return `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ` +
    `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;
}

/**
 * Format a log message with optional metadata.
 * @param {string} message - The log message
 * @param {object} [meta] - Optional metadata to append
 * @returns {string} Formatted message
 */
function formatMessage(message, meta) {
  if (meta && typeof meta === 'object') {
    return `${message} ${JSON.stringify(meta)}`;
  }
  return message;
}

class WebDashProject {
  /**
   * Create a new WebDashProject.
   * @param {string} projectName - Name for the log directory
   */
  constructor(projectName) {
    this._projectName = projectName;
    this._logDirectory = null;
    this._logFiles = {};
    this._logPaths = {};
    this._ended = false;

    this._openLogFiles();

    // Register cleanup on process exit
    this._exitHandler = () => this.end();
    process.on('exit', this._exitHandler);
    process.on('SIGINT', this._exitHandler);
    process.on('SIGTERM', this._exitHandler);
  }

  /**
   * Get the log directory path.
   * @private
   */
  _getLogDirectory() {
    const envRoot = process.env.WEBDASH;

    if (envRoot) {
      return path.join(envRoot, TEMPORARY_STORAGE_DIR, LOGGING_SUBDIR, this._projectName);
    }
    // Fallback: use current directory
    return path.join(process.cwd(), 'logs', this._projectName);
  }

  /**
   * Open all log files.
   * @private
   */
  _openLogFiles() {
    this._logDirectory = this._getLogDirectory();

    // Create directory if it doesn't exist
    fs.mkdirSync(this._logDirectory, { recursive: true });

    for (const level of LOG_LEVELS) {
      const filename = `${LOG_FILENAME_PREFIX}${level}.txt`;
      const filePath = path.join(this._logDirectory, filename);
      this._logPaths[level] = filePath;

      // Check for rotation before opening
      this._checkAndRotate(level);

      // Open in append mode
      try {
        this._logFiles[level] = fs.openSync(filePath, 'a');
      } catch (e) {
        console.error(`WebDashProject: Failed to open log file: ${filePath}: ${e.message}`);
      }
    }
  }

  /**
   * Close all log files.
   * @private
   */
  _closeLogFiles() {
    for (const level of LOG_LEVELS) {
      const fd = this._logFiles[level];
      if (fd !== undefined) {
        try {
          fs.closeSync(fd);
        } catch (e) {
          // Ignore close errors
        }
      }
    }
    this._logFiles = {};
  }

  /**
   * Check if log file needs rotation and rotate if necessary.
   * @param {string} level - Log level
   * @private
   */
  _checkAndRotate(level) {
    const filePath = this._logPaths[level];
    if (!filePath || !fs.existsSync(filePath)) {
      return;
    }

    try {
      const stats = fs.statSync(filePath);
      if (stats.size >= MAX_LOG_FILE_SIZE_BYTES) {
        // Close file if open
        if (this._logFiles[level] !== undefined) {
          fs.closeSync(this._logFiles[level]);
          delete this._logFiles[level];
        }

        // Rotate: rename to previous.<filename>
        const previousPath = path.join(
          path.dirname(filePath),
          `${LOG_ROTATION_PREFIX}${path.basename(filePath)}`
        );

        if (fs.existsSync(previousPath)) {
          fs.unlinkSync(previousPath);
        }
        fs.renameSync(filePath, previousPath);

        // Reopen fresh file with rotation message
        this._logFiles[level] = fs.openSync(filePath, 'a');
        const msg = `${getTimestamp()}: Log rotated (previous file exceeded ${MAX_LOG_FILE_SIZE_BYTES / (1024 * 1024)} MB).\n`;
        fs.writeSync(this._logFiles[level], msg);
      }
    } catch (e) {
      // Ignore rotation errors
    }
  }

  /**
   * Write a log message.
   * @param {string} level - Log level
   * @param {string} message - Log message
   * @param {object} [meta] - Optional metadata
   * @private
   */
  _log(level, message, meta) {
    if (this._ended) {
      return;
    }

    const fd = this._logFiles[level];
    if (fd === undefined) {
      return;
    }

    try {
      const formatted = formatMessage(message, meta);
      const line = `${getTimestamp()}: ${formatted}\n`;
      fs.writeSync(fd, line);
    } catch (e) {
      // Ignore write errors
    }
  }

  /**
   * Log a debug message.
   * @param {string} message
   * @param {object} [meta]
   */
  debug(message, meta) {
    this._log('debug', message, meta);
  }

  /**
   * Log an info message.
   * @param {string} message
   * @param {object} [meta]
   */
  info(message, meta) {
    this._log('info', message, meta);
  }

  /**
   * Log a warning message.
   * @param {string} message
   * @param {object} [meta]
   */
  warn(message, meta) {
    this._log('warn', message, meta);
  }

  /**
   * Log an error message.
   * @param {string} message
   * @param {object} [meta]
   */
  error(message, meta) {
    this._log('error', message, meta);
  }

  /**
   * Log a notification (separate file, opened/closed each call).
   * @param {string} message
   * @param {object} [meta]
   */
  notify(message, meta) {
    try {
      const filePath = path.join(this._logDirectory, `${LOG_FILENAME_PREFIX}notify.txt`);
      const formatted = formatMessage(message, meta);
      const line = `${getTimestamp()}: ${formatted}\n`;
      fs.appendFileSync(filePath, line);
    } catch (e) {
      // Ignore write errors
    }
  }

  /**
   * Flush log buffers (no-op for sync writes, but included for API compatibility).
   */
  flush() {
    // Node.js writeSync is synchronous, so nothing to flush
  }

  /**
   * Close all log files and mark project as ended.
   * Safe to call multiple times.
   */
  end() {
    if (this._ended) {
      return;
    }

    this._closeLogFiles();
    this._ended = true;

    // Remove exit handlers
    process.removeListener('exit', this._exitHandler);
    process.removeListener('SIGINT', this._exitHandler);
    process.removeListener('SIGTERM', this._exitHandler);
  }

  /**
   * Get the project name.
   * @returns {string}
   */
  get name() {
    return this._projectName;
  }

  /**
   * Check if project is still active.
   * @returns {boolean}
   */
  get isActive() {
    return !this._ended;
  }
}

// Global project instance management
let globalProject = null;

/**
 * Initialize the global project instance.
 * @param {string} projectName - Name for the log directory
 * @returns {WebDashProject}
 */
function initGlobal(projectName) {
  if (globalProject) {
    globalProject.end();
  }
  globalProject = new WebDashProject(projectName);
  return globalProject;
}

/**
 * Get the global project instance.
 * @returns {WebDashProject|null}
 */
function getGlobal() {
  return globalProject;
}

/**
 * End the global project.
 */
function endGlobal() {
  if (globalProject) {
    globalProject.end();
    globalProject = null;
  }
}

module.exports = {
  WebDashProject,
  initGlobal,
  getGlobal,
  endGlobal
};
