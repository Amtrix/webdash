#!/usr/bin/env node
/**
 * WebDash logging test - JavaScript
 * Tests the webdash Node.js logging API with efficient file handling.
 */

const path = require('path');
const { WebDashProject } = require(path.join(process.env.WEBDASH, 'src/lib/_webdash/executor/bindings/node/project'));

const FILENAME = 'webdash-logtests-javascript.js';

// Create a project - opens log files once
const project = new WebDashProject('webdash-logtests');

// Log at all levels - files already open, so this is fast!
project.debug(`DEBUG log from ${FILENAME}`);
project.info(`INFO log from ${FILENAME}`);
project.warn(`WARN log from ${FILENAME}`);
project.error(`ERROR log from ${FILENAME}`);

// End the project (flushes and closes files)
project.end();

console.log('JavaScript logging test complete - logged to webdash-logtests');
