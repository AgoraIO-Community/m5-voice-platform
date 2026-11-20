#!/usr/bin/env python3
"""Restore the exact bundled component sources; no network or packages needed."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile


PROJECT = Path(__file__).resolve().parents[1]
VENDOR = PROJECT / "vendor"


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_files(directory):
    """Git administration files are not part of the pinned source snapshot."""
    result = {}
    for path in sorted(directory.rglob("*")):
        relative = path.relative_to(directory)
        if ".git" in relative.parts:
            continue
        if path.is_symlink():
            raise ValueError(f"Unexpected source symlink: {path}")
        if path.is_file():
            result[relative.as_posix()] = sha256_file(path)
    return result


def verify_existing(component, directory):
    if source_files(directory) != component["files_sha256"]:
        raise ValueError(
            f"{directory} differs from the pinned snapshot. Preserve your edits, "
            "then move that directory aside before restoring dependencies."
        )


def extract_component(component, components):
    # Extract into a sibling temporary directory and rename only after every
    # file hash matches. Never replace a developer's existing component tree.
    with tempfile.TemporaryDirectory(prefix=".bootstrap-", dir=components) as temporary:
        stage = Path(temporary)
        with tarfile.open(VENDOR / component["archive"], "r:xz") as archive:
            for member in archive.getmembers():
                path = PurePosixPath(member.name)
                if (
                    path.is_absolute()
                    or ".." in path.parts
                    or not path.parts
                    or path.parts[0] != component["name"]
                    or not (member.isdir() or member.isfile())
                ):
                    raise ValueError(f"Unsafe archive member: {member.name}")
                destination = stage.joinpath(*path.parts)
                if member.isdir():
                    destination.mkdir(parents=True, exist_ok=True)
                else:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with archive.extractfile(member) as source, destination.open("xb") as target:
                        shutil.copyfileobj(source, target)
                    destination.chmod(0o755 if member.mode & 0o111 else 0o644)
        restored = stage / component["name"]
        verify_existing(component, restored)
        restored.rename(components / component["name"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Verify archives and installed sources without changing files")
    parser.add_argument("--components-dir", type=Path, default=PROJECT / "components", help="Alternate restoration directory")
    args = parser.parse_args()
    manifest = json.loads((VENDOR / "manifest.json").read_text(encoding="utf-8"))
    if manifest["format_version"] != 1:
        raise ValueError("Unsupported dependency manifest format")
    components = args.components_dir.resolve()
    pending = []
    # Validate all archives and existing trees before any extraction.
    for component in manifest["components"]:
        archive = VENDOR / component["archive"]
        if sha256_file(archive) != component["archive_sha256"]:
            raise ValueError(f"Archive SHA-256 mismatch: {archive}")
        destination = components / component["name"]
        if destination.exists():
            verify_existing(component, destination)
            print(f"Verified {component['name']} {component['declared_version']}")
        elif args.check:
            raise ValueError(f"Missing component: {destination}")
        else:
            pending.append(component)
    if pending:
        components.mkdir(parents=True, exist_ok=True)
    for component in pending:
        extract_component(component, components)
        print(f"Restored {component['name']} {component['declared_version']}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        raise SystemExit(f"Dependency bootstrap failed: {error}") from error
