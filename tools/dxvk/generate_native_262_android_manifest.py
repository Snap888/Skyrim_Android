#!/usr/bin/env python3
"""Extract a reviewed Android ARM64 source inventory from pinned DXVK 2.6.2 Meson files.

This is deliberately an inventory generator, not a CMake build generator: Meson
conditionals, generated shaders, platform libraries and linker scripts still
need explicit Android review before any source is compiled.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ARRAYS = {
    "d3d9": ("src/d3d9/meson.build", "d3d9_src"),
    "dxvk": ("src/dxvk/meson.build", "dxvk_src"),
    "dxso": ("src/dxso/meson.build", "dxso_src"),
    "util": ("src/util/meson.build", "util_src"),
    "vulkan": ("src/vulkan/meson.build", "vkcommon_src"),
    "wsi": ("src/wsi/meson.build", "wsi_src"),
}

ANDROID_WSI = [
    "src/wsi/wsi_edid.cpp",
    "src/wsi/wsi_platform.cpp",
    "src/wsi/sdl2/wsi_monitor_sdl2.cpp",
    "src/wsi/sdl2/wsi_platform_sdl2.cpp",
    "src/wsi/sdl2/wsi_window_sdl2.cpp",
]


def extract_array(root: Path, relative: str, name: str) -> list[str]:
    text = (root / relative).read_text(encoding="utf-8")
    match = re.search(
        rf"(?m)^\s*{re.escape(name)}\s*=\s*\[(.*?)^\s*\]",
        text,
        flags=re.DOTALL,
    )
    if not match:
        raise ValueError(f"Meson array {name!r} not found in {relative}")
    # Meson source arrays in these files contain quoted paths, one per line.
    return re.findall(r"""['"]([^'"]+\.(?:cpp|c))['"]""", match.group(1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()

    groups: dict[str, list[str]] = {}
    errors: list[str] = []
    for group, (meson_file, array_name) in ARRAYS.items():
        values = extract_array(root, meson_file, array_name)
        prefix = Path(meson_file).parent
        paths = [str(prefix / value) for value in values]
        if not paths:
            errors.append(f"{group}: empty source list")
        for path in paths:
            if not (root / path).is_file():
                errors.append(f"{group}: missing source {path}")
        groups[group] = paths

    for path in ANDROID_WSI:
        if not (root / path).is_file():
            errors.append(f"android_wsi: missing source {path}")
    groups["android_wsi"] = ANDROID_WSI

    if errors:
        raise SystemExit("\n".join(errors))

    # Preserve source order and make accidental duplicate compilation visible.
    flattened = [p for values in groups.values() for p in values]
    duplicates = sorted({p for p in flattened if flattened.count(p) > 1})
    manifest = {
        "schema": "dxvk-native-android-source-manifest-v1",
        "upstream": "doitsujin/dxvk",
        "revision": "9d6f54a1ade20d1d27dd421024717a636f3d8c68",
        "version": "2.6.2",
        "status": "SOURCE_INVENTORY_ONLY_NOT_A_BUILD_TARGET",
        "groups": groups,
        "counts": {key: len(value) for key, value in groups.items()},
        "duplicate_sources": duplicates,
        "review_notes": [
            "Meson source arrays are authoritative inventory, not a complete CMake port.",
            "Android uses SDL2 WSI only; SDL3, GLFW and Win32 sources are excluded.",
            "Generated GLSL headers, resource generation, compile definitions, dependencies and link flags require separate review.",
            "Do not package this inventory until an Android build and ABI-pair verification pass.",
        ],
    }
    rendered = json.dumps(manifest, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    print(
        "DXVK_NATIVE_262_SOURCE_INVENTORY_OK "
        + " ".join(f"{key}={len(value)}" for key, value in groups.items())
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
