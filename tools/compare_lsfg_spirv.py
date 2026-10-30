#!/usr/bin/env python3
"""Compare extracted LSFG SPIR-V modules without modifying the shader cache.

Only exact byte identity is established by SHA-256/equality. Different binaries
may still implement equivalent algorithms; matching interfaces are not enough.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
from pathlib import Path

DEFAULT_PAIRS = [
    (339, 388), (340, 389), (341, 390), (342, 391),
    (343, 392), (344, 393), (345, 394), (346, 395),
    (347, 396), (348, 397), (349, 398), (350, 399), (351, 400),
]
SPIRV_MAGIC = 0x07230203


def inspect(path: Path) -> dict:
    if not path.is_file():
        return {"path": str(path), "status": "missing"}
    data = path.read_bytes()
    result = {
        "path": str(path),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "status": "ok",
    }
    if len(data) < 20 or len(data) % 4:
        result["status"] = "invalid-size"
        return result
    magic, version, generator, bound, schema = struct.unpack_from("<5I", data)
    result["header"] = {
        "magic": f"0x{magic:08x}",
        "version": f"{version >> 16}.{(version >> 8) & 0xff}",
        "generator": generator,
        "id_bound": bound,
        "schema": schema,
    }
    if magic != SPIRV_MAGIC or schema != 0:
        result["status"] = "invalid-header"
    return result


def disassemble(path: Path, tool: str) -> dict:
    proc = subprocess.run(
        [tool, str(path)], check=False, capture_output=True, text=True
    )
    if proc.returncode:
        return {"status": "failed", "returncode": proc.returncode,
                "stderr": proc.stderr[-2000:]}
    text = proc.stdout
    return {
        "status": "ok",
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "bytes": len(text.encode("utf-8")),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path, help="directory containing N.spv files")
    parser.add_argument("--pairs", nargs="*", metavar="A:B",
                        help="ID pairs; default compares the candidate +49/+98 variants")
    parser.add_argument("--spirv-dis", default="spirv-dis",
                        help="SPIRV-Tools disassembler executable (optional)")
    args = parser.parse_args()

    pairs = DEFAULT_PAIRS
    if args.pairs:
        pairs = []
        for value in args.pairs:
            try:
                left, right = value.split(":", 1)
                pairs.append((int(left), int(right)))
            except ValueError:
                parser.error(f"invalid pair {value!r}; expected A:B")

    dis_tool = shutil.which(args.spirv_dis)
    reports = []
    for left, right in pairs:
        a_path, b_path = args.cache / f"{left}.spv", args.cache / f"{right}.spv"
        a, b = inspect(a_path), inspect(b_path)
        row = {"pair": [left, right], "left": a, "right": b}
        if a["status"] != "ok" or b["status"] != "ok":
            row["comparison"] = "unavailable"
        else:
            row["comparison"] = "byte-identical" if a["sha256"] == b["sha256"] else "different"
            if dis_tool:
                row["left_disassembly"] = disassemble(a_path, dis_tool)
                row["right_disassembly"] = disassemble(b_path, dis_tool)
                da, db = row["left_disassembly"], row["right_disassembly"]
                if da["status"] == db["status"] == "ok":
                    row["disassembly_comparison"] = (
                        "text-identical" if da["sha256"] == db["sha256"] else "text-different"
                    )
                else:
                    row["disassembly_comparison"] = "unavailable"
        reports.append(row)

    print(json.dumps({
        "cache": str(args.cache),
        "spirv_dis_available": bool(dis_tool),
        "warning": "Byte or disassembly equality does not prove runtime selection or algorithmic equivalence.",
        "results": reports,
    }, indent=2))
    return 0 if all(r["comparison"] != "unavailable" for r in reports) else 2


if __name__ == "__main__":
    raise SystemExit(main())
