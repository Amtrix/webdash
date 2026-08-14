#!/usr/bin/env python3
"""
Webdash Backup

Selectively backup files and directories that shouldn't be in git
(credentials, tokens, local configs, etc.)

Config source:
    - webdash.profile.jsonc "backup" section
"""

import argparse
import filecmp
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROFILE_FILENAME = "webdash.profile.jsonc"

# Default webdash root (3 levels up from src/static/webdash-file-backup)
WEBDASH_ROOT = os.environ.get("WEBDASH") or os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", ".."))


def _strip_jsonc(text):
    """
    Strip JSONC-only syntax ("//" line comments, "/* */" block comments,
    trailing commas) so the result can be parsed by the standard `json`
    module. Respects string literals (a "//"/"/*" inside a quoted string,
    e.g. a "https://" URL, is left untouched). Mirrors the C++ core's native
    nlohmann/json ignore_comments/ignore_trailing_commas support.
    """
    result = []
    i = 0
    n = len(text)
    in_string = False
    escape = False

    while i < n:
        c = text[i]

        if in_string:
            result.append(c)
            if escape:
                escape = False
            elif c == '\\':
                escape = True
            elif c == '"':
                in_string = False
            i += 1
            continue

        if c == '"':
            in_string = True
            result.append(c)
            i += 1
            continue

        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j == -1 else j
            continue

        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j == -1 else j + 2
            continue

        result.append(c)
        i += 1

    return re.sub(r',(\s*[}\]])', r'\1', ''.join(result))


def load_config_from_profile():
    """Load backup configuration from webdash.profile.jsonc's 'backup' section."""
    profile_path = os.path.join(WEBDASH_ROOT, PROFILE_FILENAME)

    if not os.path.exists(profile_path):
        print(f"Error: {profile_path} not found.")
        sys.exit(1)

    with open(profile_path, 'r') as f:
        profile = json.loads(_strip_jsonc(f.read()))

    backup_section = profile.get("backup")
    if not backup_section:
        print(f"Error: No 'backup' section found in {PROFILE_FILENAME}.")
        sys.exit(1)

    storage_dir = backup_section.get("storage-location-directory")
    if not storage_dir:
        print(f"Error: 'storage-location-directory' is required in the backup section.")
        sys.exit(1)

    # Convert profile format to internal config format
    config = {
        "backup-location": storage_dir,
        "sync-source": []
    }

    mount = backup_section.get("mount")
    if mount:
        config["mount"] = mount

    for f_path in backup_section.get("files", []):
        config["sync-source"].append({"file": f_path})

    for d_path in backup_section.get("directories", []):
        config["sync-source"].append({"dir": d_path})

    return config


def ensure_mount(config):
    """Ensure the backup mount point is accessible, remounting if stale."""
    mount = config.get("mount")
    if not mount:
        return

    point = mount.get("point")
    device = mount.get("device")
    fs_type = mount.get("type")

    if not point or not device:
        return

    def is_mounted():
        """Check if something is actually mounted at the mount point."""
        try:
            with open("/proc/mounts", "r") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) >= 2 and parts[1] == point:
                        # Also verify the mount is responsive
                        os.listdir(point)
                        return True
        except OSError:
            pass
        return False

    if is_mounted():
        return  # Mount is healthy

    print(f"Mount {point} is not mounted or stale. Attempting remount...")

    # Try unmounting first (stale mount)
    subprocess.run(["sudo", "umount", point], capture_output=True)

    # Ensure mount point directory exists
    os.makedirs(point, exist_ok=True)

    # Mount
    mount_cmd = ["sudo", "mount", "-t", fs_type, device, point] if fs_type else \
                ["sudo", "mount", device, point]
    result = subprocess.run(mount_cmd, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"  Failed to remount {device} at {point}")
        if result.stderr.strip():
            print(f"  ({result.stderr.strip()})")
        sys.exit(1)

    # Verify
    try:
        os.listdir(point)
        print(f"  Remounted {device} at {point} successfully.")
    except OSError as e:
        print(f"  Remount appeared to succeed but {point} is still inaccessible.")
        print(f"  ({e})")
        sys.exit(1)


def backup_file(src_path, backup_dir, relative_path, dry_run=False):
    """Backup a single file."""
    dest_path = os.path.join(backup_dir, relative_path)
    dest_dir = os.path.dirname(dest_path)

    if not os.path.exists(src_path):
        print(f"  [SKIP] {relative_path} (not found)")
        return False

    if dry_run:
        print(f"  [DRY RUN] {relative_path}")
        return True

    os.makedirs(dest_dir, exist_ok=True)

    # Check if file is unchanged
    if os.path.exists(dest_path) and filecmp.cmp(src_path, dest_path, shallow=False):
        print(f"  [SKIPPED] {relative_path} (unchanged)")
        return True

    shutil.copyfile(src_path, dest_path)
    print(f"  [OK] {relative_path}")
    return True


def backup_directory(src_path, backup_dir, relative_path, dry_run=False):
    """Backup a directory recursively."""
    dest_path = os.path.join(backup_dir, relative_path)

    if not os.path.exists(src_path):
        print(f"  [SKIP] {relative_path}/ (not found)")
        return False

    if dry_run:
        file_count = sum(len(files) for _, _, files in os.walk(src_path))
        print(f"  [DRY RUN] {relative_path}/ ({file_count} files)")
        return True

    # Check if directory is unchanged
    if os.path.exists(dest_path):
        comparison = filecmp.dircmp(src_path, dest_path)
        if not comparison.left_only and not comparison.right_only and not comparison.diff_files:
            # Check subdirs recursively
            def dirs_equal(dcmp):
                if dcmp.left_only or dcmp.right_only or dcmp.diff_files:
                    return False
                return all(dirs_equal(sub) for sub in dcmp.subdirs.values())
            if dirs_equal(comparison):
                file_count = sum(len(files) for _, _, files in os.walk(src_path))
                print(f"  [SKIPPED] {relative_path}/ ({file_count} files, unchanged)")
                return True
        # Has changes, remove old backup
        shutil.rmtree(dest_path)

    # Manual copy to avoid permission issues on Google Drive etc
    file_count = 0
    for root, dirs, files in os.walk(src_path):
        rel_root = os.path.relpath(root, src_path)
        dest_root = os.path.join(dest_path, rel_root) if rel_root != '.' else dest_path
        os.makedirs(dest_root, exist_ok=True)
        for f in files:
            src_file = os.path.join(root, f)
            dest_file = os.path.join(dest_root, f)
            shutil.copyfile(src_file, dest_file)
            file_count += 1

    print(f"  [OK] {relative_path}/ ({file_count} files)")
    return True


def run_backup(dry_run=False):
    """Run the backup process."""
    config = load_config_from_profile()
    ensure_mount(config)

    backup_location = config.get("backup-location")
    if not backup_location:
        print(f"Error: 'storage-location-directory' not set in {PROFILE_FILENAME} (backup section)")
        sys.exit(1)

    # Expand ~ and make absolute
    backup_location = os.path.expanduser(backup_location)
    if not os.path.isabs(backup_location):
        backup_location = os.path.abspath(backup_location)

    if not os.path.exists(backup_location):
        try:
            os.makedirs(backup_location, exist_ok=True)
        except OSError as e:
            print(f"Error: Backup location is not accessible: {backup_location}")
            print(f"  ({e})")
            sys.exit(1)

    sync_sources = config.get("sync-source", [])
    if not sync_sources:
        print("No sources configured in 'sync-source'")
        return

    print(f"Backup location: {backup_location}")
    print(f"Webdash root: {WEBDASH_ROOT}")
    print(f"Sources: {len(sync_sources)}")
    print()

    if dry_run:
        print("=== DRY RUN ===\n")

    success = 0
    skipped = 0

    for source in sync_sources:
        if "file" in source:
            relative_path = source["file"]
            src_path = os.path.join(WEBDASH_ROOT, relative_path)
            if backup_file(src_path, backup_location, relative_path, dry_run):
                success += 1
            else:
                skipped += 1

        elif "dir" in source:
            relative_path = source["dir"]
            src_path = os.path.join(WEBDASH_ROOT, relative_path)
            if backup_directory(src_path, backup_location, relative_path, dry_run):
                success += 1
            else:
                skipped += 1

    print()
    print(f"Backed up: {success}, Skipped: {skipped}")


def run_restore(dry_run=False):
    """Restore files from backup."""
    config = load_config_from_profile()
    ensure_mount(config)

    backup_location = config.get("backup-location")
    if not backup_location:
        print(f"Error: 'storage-location-directory' not set in {PROFILE_FILENAME} (backup section)")
        sys.exit(1)

    backup_location = os.path.expanduser(backup_location)
    if not os.path.isabs(backup_location):
        backup_location = os.path.abspath(backup_location)

    if not os.path.exists(backup_location):
        print(f"Error: Backup location does not exist: {backup_location}")
        sys.exit(1)

    sync_sources = config.get("sync-source", [])
    if not sync_sources:
        print("No sources configured in 'sync-source'")
        return

    print(f"Restoring from: {backup_location}")
    print(f"Webdash root: {WEBDASH_ROOT}")
    print(f"Sources: {len(sync_sources)}")
    print()

    if dry_run:
        print("=== DRY RUN ===\n")

    success = 0
    skipped = 0

    for source in sync_sources:
        if "file" in source:
            relative_path = source["file"]
            src_path = os.path.join(backup_location, relative_path)
            dest_path = os.path.join(WEBDASH_ROOT, relative_path)

            if not os.path.exists(src_path):
                print(f"  [SKIP] {relative_path} (not in backup)")
                skipped += 1
                continue

            if dry_run:
                print(f"  [DRY RUN] {relative_path}")
            else:
                os.makedirs(os.path.dirname(dest_path), exist_ok=True)
                shutil.copy(src_path, dest_path)
                print(f"  [OK] {relative_path}")
            success += 1

        elif "dir" in source:
            relative_path = source["dir"]
            src_path = os.path.join(backup_location, relative_path)
            dest_path = os.path.join(WEBDASH_ROOT, relative_path)

            if not os.path.exists(src_path):
                print(f"  [SKIP] {relative_path}/ (not in backup)")
                skipped += 1
                continue

            if dry_run:
                file_count = sum(len(files) for _, _, files in os.walk(src_path))
                print(f"  [DRY RUN] {relative_path}/ ({file_count} files)")
            else:
                if os.path.exists(dest_path):
                    shutil.rmtree(dest_path)
                shutil.copytree(src_path, dest_path)
                file_count = sum(len(files) for _, _, files in os.walk(dest_path))
                print(f"  [OK] {relative_path}/ ({file_count} files)")
            success += 1

    print()
    print(f"Restored: {success}, Skipped: {skipped}")


def main():
    parser = argparse.ArgumentParser(
        description='Backup/restore files that should not be in git'
    )
    parser.add_argument('action', nargs='?', default='backup',
                        choices=['backup', 'restore'],
                        help='Action to perform (default: backup)')
    parser.add_argument('--dry-run', action='store_true',
                        help='Show what would be done without doing it')
    # Backward compatibility: older webdash clients pass --from-profile.
    # This tool now always reads from webdash.profile.jsonc; the flag is ignored.
    parser.add_argument('--from-profile', action='store_true',
                        help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.action == 'backup':
        run_backup(dry_run=args.dry_run)
    elif args.action == 'restore':
        run_restore(dry_run=args.dry_run)


if __name__ == '__main__':
    main()
