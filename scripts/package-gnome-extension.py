#!/usr/bin/env python3
"""Build deterministic legacy and ES-module Deskpal GNOME extension ZIPs."""

import argparse
import json
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "gnome-extension" / "indicator@deskpal.local"
UUID = "indicator@deskpal.local"
FIXED_TIME = (2026, 1, 1, 0, 0, 0)

LEGACY_IMPORTS = """const {Clutter, Gio, GLib, Meta, St} = imports.gi;
const Main = imports.ui.main;"""
MODERN_IMPORTS = """import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';"""
LEGACY_CLASS = "class DeskpalIndicatorExtension {"
MODERN_CLASS = "export default class DeskpalIndicatorExtension extends Extension {"
LEGACY_INIT = """
function init() {
    return new DeskpalIndicatorExtension();
}
"""


def modern_source(source: str) -> str:
    if source.count(LEGACY_IMPORTS) != 1:
        raise ValueError("legacy import block changed")
    if source.count(LEGACY_CLASS) != 1 or source.count(LEGACY_INIT) != 1:
        raise ValueError("legacy extension entry point changed")
    return (
        source.replace("/* exported init */\n", "", 1)
        .replace(LEGACY_IMPORTS, MODERN_IMPORTS, 1)
        .replace(LEGACY_CLASS, MODERN_CLASS, 1)
        .replace(LEGACY_INIT, "\n", 1)
    )


def zip_bytes(path: Path, files: dict[str, bytes]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, files[name])


def metadata_for(shell_versions: list[str]) -> bytes:
    metadata = json.loads((SOURCE / "metadata.json").read_text())
    if metadata.get("uuid") != UUID:
        raise ValueError("unexpected extension UUID")
    metadata["shell-version"] = shell_versions
    return (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()


def build(output: Path) -> list[Path]:
    legacy = (SOURCE / "extension.js").read_text()
    common = {"stylesheet.css": (SOURCE / "stylesheet.css").read_bytes()}
    artifacts = [
        (
            output / "deskpal-shell-extension-gnome42.zip",
            legacy.encode(),
            ["42"],
        ),
        (
            output / "deskpal-shell-extension-gnome45-50.zip",
            modern_source(legacy).encode(),
            ["45", "46", "47", "48", "49", "50"],
        ),
    ]
    results = []
    for path, source, versions in artifacts:
        zip_bytes(path, {
            **common,
            "extension.js": source,
            "metadata.json": metadata_for(versions),
        })
        results.append(path)
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    for artifact in build(args.output.resolve()):
        print(artifact)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
