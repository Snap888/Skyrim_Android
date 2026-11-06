#!/usr/bin/env python3
"""Extract a reviewed Android ARM64 source inventory from pinned DXVK 2.6.2 Meson files.

This is deliberately an inventory generator, not a CMake build generator. Meson
conditionals, generated shaders, platform libraries and linker scripts still
need explicit Android review before any source is compiled.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

REVISION = "9d6f54a1ade20d1d27dd421024717a636f3d8c68"

ARRAYS = {
    "d3d9": ("src/d3d9/meson.build", "d3d9_src"),
    "dxvk": ("src/dxvk/meson.build", "dxvk_src"),
    "dxso": ("src/dxso/meson.build", "dxso_src"),
    "util": ("src/util/meson.build", "util_src"),
    "spirv": ("src/spirv/meson.build", "spirv_src"),
    "vulkan": ("src/vulkan/meson.build", "vkcommon_src"),
    "wsi_upstream": ("src/wsi/meson.build", "wsi_src"),
}

ANDROID_WSI = {
    "wsi_edid.cpp",
    "wsi_platform.cpp",
    "sdl2/wsi_monitor_sdl2.cpp",
    "sdl2/wsi_platform_sdl2.cpp",
    "sdl2/wsi_window_sdl2.cpp",
}


def extract_array(root: Path, relative: str, name: str) -> list[str]:
    text = (root / relative).read_text(encoding="utf-8")
    match = re.search(
        rf"(?m)^\s*{re.escape(name)}\s*=\s*(?:files\(\s*)?\[(.*?)^\s*\]",
        text,
        flags=re.DOTALL,
    )
    if not match:
        raise ValueError(f"Meson array {name!r} not found in {relative}")
    values = re.findall(r"""['"]([^'"]+\.(?:cpp|c))['"]""", match.group(1))
    if not values:
        raise ValueError(f"Meson array {name!r} in {relative} contains no C/C++ sources")
    return values


def extract_shader_array(root: Path, relative: str, name: str) -> list[str]:
    text = (root / relative).read_text(encoding="utf-8")
    match = re.search(
        rf"(?m)^\s*{re.escape(name)}\s*=\s*(?:files\(\s*)?\[(.*?)^\s*\]",
        text,
        flags=re.DOTALL,
    )
    if not match:
        raise ValueError(f"Meson shader array {name!r} not found in {relative}")
    values = re.findall(r"""['"]([^'"]+\.(?:comp|frag|vert|geom))['"]""", match.group(1))
    if not values:
        raise ValueError(f"Meson shader array {name!r} in {relative} contains no shader sources")
    return values


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

        if group == "wsi_upstream":
            # Upstream mixes common, SDL2, SDL3, GLFW and Win32 sources.
            relative = [value for value in values if value in ANDROID_WSI]
            missing = sorted(ANDROID_WSI - set(relative))
            if missing:
                errors.append(f"wsi_upstream: required Android WSI entries missing: {missing}")
            groups["wsi_common"] = [
                str(prefix / value) for value in relative
                if value in {"wsi_edid.cpp", "wsi_platform.cpp"}
            ]
            groups["android_wsi"] = [
                str(prefix / value) for value in relative
                if value.startswith("sdl2/")
            ]
            groups["wsi_excluded_desktop"] = [
                str(prefix / value) for value in values if value not in ANDROID_WSI
            ]
            # Validate excluded upstream files too, so the inventory reflects
            # the complete pinned Meson array rather than a hand-written subset.
            for path in paths:
                if not (root / path).is_file():
                    errors.append(f"{group}: missing upstream source {path}")
            continue

        if not paths:
            errors.append(f"{group}: empty source list")
        for path in paths:
            if not (root / path).is_file():
                errors.append(f"{group}: missing source {path}")
        groups[group] = paths

    # Inventory GLSL inputs separately from C/C++ translation units.
    for group, meson_file, array_name in (
        ("d3d9_shaders", "src/d3d9/meson.build", "d3d9_shaders"),
        ("dxvk_shaders", "src/dxvk/meson.build", "dxvk_shaders"),
    ):
        values = extract_shader_array(root, meson_file, array_name)
        prefix = Path(meson_file).parent
        paths = [str(prefix / value) for value in values]
        for path in paths:
            if not (root / path).is_file():
                errors.append(f"{group}: missing shader source {path}")
        groups[group] = paths

    # Project-owned Android surface lifetime adapter, copied into the pinned
    # upstream tree by apply_native_262_android_port.py before inventory.
    overlay_source = "src/wsi/sdl2/wsi_native_surface.cpp"
    if not (root / overlay_source).is_file():
        errors.append(f"android_wsi_overlay: missing project adapter {overlay_source}")
    groups["android_wsi_overlay"] = [overlay_source]

    for group in ("wsi_common", "android_wsi", "android_wsi_overlay"):
        if not groups.get(group):
            errors.append(f"{group}: empty Android source list")

    if errors:
        raise SystemExit("\n".join(errors))

    build_groups = [
        "d3d9", "dxvk", "dxso", "util", "spirv", "vulkan", "wsi_common", "android_wsi", "android_wsi_overlay"
    ]
    flattened = [p for key in build_groups for p in groups[key]]
    duplicates = sorted({p for p in flattened if flattened.count(p) > 1})
    if duplicates:
        raise SystemExit("duplicate Android build sources: " + ", ".join(duplicates))

    manifest = {
        "schema": "dxvk-native-android-source-manifest-v1",
        "upstream": "doitsujin/dxvk",
        "revision": REVISION,
        "version": "2.6.2",
        "status": "SOURCE_INVENTORY_ONLY_NOT_A_BUILD_TARGET",
        "build_groups": build_groups,
        "groups": groups,
        "counts": {key: len(value) for key, value in groups.items()},
        "duplicate_android_build_sources": duplicates,
        "review_notes": [
            "Meson source arrays are an inventory, not a complete Android CMake target.",
            "Android candidate includes common WSI plus SDL2 WSI only.",
            "SDL3, GLFW and Win32 WSI sources are inventoried separately and excluded.",
            "GLSL shader inputs are inventoried separately; generated headers, resources, compile definitions, dependencies and link flags still require separate review.",
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
