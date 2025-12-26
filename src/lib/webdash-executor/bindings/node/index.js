/**
 * WebDash Node.js bindings
 *
 * Provides access to WebDash framework functionality from Node.js:
 * - Logging (info, error, warn, debug, notify)
 * - WebDash root directory access
 *
 * @example
 * const webdash = require('webdash');
 *
 * webdash.log.info('Server started');
 * webdash.log.error('Connection failed');
 * webdash.log.debug('Request received', { path: '/api' });
 */

'use strict';

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

module.exports = {
    log,
    getRootDirectory,
    getProjectName,

    // Also expose raw native bindings for advanced use
    native
};
