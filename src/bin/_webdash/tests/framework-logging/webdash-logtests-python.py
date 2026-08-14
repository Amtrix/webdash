#!/usr/bin/env python3
"""
WebDash logging test - Python
Tests the webdash Python logging API with efficient file handling.
"""

import sys
import os
sys.path.insert(0, os.path.join(os.environ["WEBDASH"], "src/lib/_webdash/executor/bindings/python"))

from webdash.log import WebDashProject

FILENAME = "webdash-logtests-python.py"

# Create a project - opens log files once
project = WebDashProject("webdash-logtests")

# Log at all levels - files already open, so this is fast!
project.debug(f"DEBUG log from {FILENAME}")
project.info(f"INFO log from {FILENAME}")
project.warn(f"WARN log from {FILENAME}")
project.error(f"ERROR log from {FILENAME}")

# Destructor calls end() automatically, flushing and closing files
print(f"Python logging test complete - logged to webdash-logtests")
