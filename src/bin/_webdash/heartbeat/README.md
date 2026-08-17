# WebDash Heartbeat

Integration test for WebDash scheduled execution.

## Purpose

Verifies that the WebDash server's scheduling system works correctly by logging a timestamp every minute. Check the logs to confirm scheduled execution is running.

## Setup

```bash
# Build and install
webdash all

# Register with WebDash server for scheduled execution
webdash --config.register.add
```

## Verification

After registration, check the logs to see heartbeat entries:

```bash
cat $WEBDASH/app-temporary/logging/webdash-heartbeat/logging.info.txt
```

You should see entries like:
```
Heartbeat: 2025-12-24 15:30:00.123
Heartbeat: 2025-12-24 15:31:00.456
...
```

## Configuration

The `heartbeat` command in `webdash.config.jsonc` is configured with:
- `frequency: "60000"` — Run at most once per 60 seconds (60000 ms)

## Manual Run

To run manually (for testing):
```bash
webdash-heartbeat
```
