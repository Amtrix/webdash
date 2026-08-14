#!/usr/bin/env ts-node
/**
 * WebDash logging test - TypeScript
 * Tests the webdash Node.js logging API with efficient file handling.
 */

// eslint-disable-next-line @typescript-eslint/no-var-requires
import * as path from 'path';
const { WebDashProject } = require(path.join(process.env.WEBDASH as string, 'src/lib/_webdash/executor/bindings/node/project'));

const FILENAME: string = 'webdash-logtests-typescript.ts';

// Create a project - opens log files once
const project = new WebDashProject('webdash-logtests');

// Log at all levels - files already open, so this is fast!
project.debug(`DEBUG log from ${FILENAME}`);
project.info(`INFO log from ${FILENAME}`);
project.warn(`WARN log from ${FILENAME}`);
project.error(`ERROR log from ${FILENAME}`);

// End the project (flushes and closes files)
project.end();

console.log('TypeScript logging test complete - logged to webdash-logtests');
