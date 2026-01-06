/**
 * WebDash Node.js bindings
 *
 * Provides access to WebDash framework functionality from Node.js:
 * - Logging (info, error, warn, debug, notify)
 * - WebDash root directory access
 * - Config value resolution (with directory traversal)
 *
 * @example
 * const webdash = require('webdash');
 *
 * webdash.log.info('Server started');
 * webdash.log.error('Connection failed');
 * webdash.log.debug('Request received', { path: '/api' });
 */

'use strict';

const fs = require('fs');
const path = require('path');

// Load the native addon
let native;
try {
  native = require('./build/Release/webdash_napi.node');
} catch (e) {
  try {
    native = require('./build/Debug/webdash_napi.node');
  } catch (e2) {
    throw new Error(
      'Failed to load webdash native bindings. ' +
      'Make sure you ran `npm run build` first.\n' +
      `Original error: ${e.message}`
    );
  }
}

const CONFIG_FILENAME = 'webdash.config.json';
const PROFILE_FILENAME = 'webdash-profile.json';

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

/**
 * Logging interface - mimics common logging libraries.
 */
const log = {
  /**
   * Log an info message.
   * @param {string} message
   * @param {object} [meta]
   */
  info(message, meta) {
    native.log('info', formatMessage(message, meta));
  },

  /**
   * Log an error message.
   * @param {string} message
   * @param {object} [meta]
   */
  error(message, meta) {
    native.log('error', formatMessage(message, meta));
  },

  /**
   * Log a warning message.
   * @param {string} message
   * @param {object} [meta]
   */
  warn(message, meta) {
    native.log('warn', formatMessage(message, meta));
  },

  /**
   * Log a debug message.
   * @param {string} message
   * @param {object} [meta]
   */
  debug(message, meta) {
    native.log('debug', formatMessage(message, meta));
  },

  /**
   * Log a notification (never rotated).
   * @param {string} message
   * @param {object} [meta]
   */
  notify(message, meta) {
    native.notify(formatMessage(message, meta));
  }
};

/**
 * Get the WebDash root directory.
 * @returns {string} Path to WebDash root
 */
function getRootDirectory() {
  return native.getRootDirectory();
}

/**
 * Get the current project name.
 * @returns {string} Project name
 */
function getProjectName() {
  return native.getProjectName();
}

/**
 * Get a value from webdash-profile.json by dot-separated key path.
 * @param {string} keyPath - Dot-separated path like "ports.my-service"
 * @returns {string|undefined} The value, or undefined if not found
 * @example
 * getProfileValue('ports.src-static-wallpaper-engine-dashboard-monitor') // "8765"
 * getProfileValue('env.WEBDASH') // resolved path
 */
function getProfileValue(keyPath) {
  return native.getProfileValue(keyPath);
}

/**
 * Get a port number from the profile's "ports" section.
 * @param {string} portKey - The port key (e.g., "src-static-wallpaper-engine-dashboard-monitor")
 * @param {number} [defaultPort] - Default port if not found
 * @returns {number} The port number
 */
function getPort(portKey, defaultPort = 0) {
  const value = getProfileValue(`ports.${portKey}`);
  if (value !== undefined) {
    return parseInt(value, 10);
  }
  return defaultPort;
}

/**
 * Get a nested value from an object using a dot-separated key path.
 * @param {object} obj - The object to traverse
 * @param {string} keyPath - Dot-separated path like "ports.my-service"
 * @returns {*} The value, or undefined if not found
 */
function getNestedValue(obj, keyPath) {
  const keys = keyPath.split('.');
  let current = obj;

  for (const key of keys) {
    if (current === null || current === undefined || typeof current !== 'object') {
      return undefined;
    }
    current = current[key];
  }

  return current;
}

/**
 * Read and parse a JSON config file.
 * @param {string} filePath - Path to JSON file
 * @returns {object|null} Parsed JSON or null if not found/invalid
 */
function readJsonFile(filePath) {
  try {
    if (fs.existsSync(filePath)) {
      return JSON.parse(fs.readFileSync(filePath, 'utf8'));
    }
  } catch (e) {
    // Ignore parse errors
  }
  return null;
}

/**
 * Resolve a config value by traversing up the directory tree.
 *
 * Lookup order:
 * 1. webdash.config.json in startDir
 * 2. webdash.config.json in parent directories (up to root)
 * 3. webdash-profile.json at WebDash root
 *
 * @param {string} keyPath - Dot-separated path like "ports.my-service"
 * @param {object} [options] - Options
 * @param {string} [options.startDir] - Directory to start searching from (default: process.cwd())
 * @param {boolean} [options.skipLocal] - Skip webdash.config.json files, only check profile
 * @returns {*} The resolved value, or undefined if not found
 *
 * @example
 * // From current directory, check webdash.config.json files then profile
 * resolveConfigValue('ports.my-service')
 *
 * // Start from a specific directory
 * resolveConfigValue('mycommands.build', { startDir: '/path/to/project' })
 *
 * // Only check webdash-profile.json
 * resolveConfigValue('env.WEBDASH', { skipLocal: true })
 */
function resolveConfigValue(keyPath, options = {}) {
  const { startDir = process.cwd(), skipLocal = false } = options;

  let webdashRoot;
  try {
    webdashRoot = native.getRootDirectory();
  } catch (e) {
    webdashRoot = process.env.WEBDASH;
  }

  // Traverse up from startDir looking for webdash.config.json
  if (!skipLocal) {
    let currentDir = path.resolve(startDir);
    const rootDir = path.parse(currentDir).root;

    while (currentDir !== rootDir) {
      // Stop if we've reached the webdash root (profile takes precedence there)
      if (webdashRoot && currentDir === path.resolve(webdashRoot)) {
        break;
      }

      const configPath = path.join(currentDir, CONFIG_FILENAME);
      const config = readJsonFile(configPath);

      if (config) {
        const value = getNestedValue(config, keyPath);
        if (value !== undefined) {
          return value;
        }
      }

      // Move to parent directory
      const parentDir = path.dirname(currentDir);
      if (parentDir === currentDir) break; // Reached filesystem root
      currentDir = parentDir;
    }
  }

  // Fall back to webdash-profile.json via native bindings
  // This handles the $.rootDir() substitutions properly
  const profileValue = native.getProfileValue(keyPath);
  if (profileValue !== undefined) {
    return profileValue;
  }

  return undefined;
}

module.exports = {
  log,
  getRootDirectory,
  getProjectName,

  // Direct profile access (webdash-profile.json only)
  getProfileValue,
  getPort,

  // Config resolution with directory traversal
  resolveConfigValue,

  // Also expose raw native bindings for advanced use
  native
};
