#!/usr/bin/env python3
"""Read-only G0 check of two built Ninja trees (testing ON and OFF).

Run explicitly, not from CTest: it requires a separate no-tests build and must
not introduce recursive builds or change the existing conformance inventory.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess


STAGES = (
    "src/backends/discovery_stage.cpp",
    "src/backends/working_set_stage.cpp",
    "src/backends/envelope_stage.cpp",
)
HARNESSES = {
    "src/backends/reference/tier2_oracle.cpp": "vg_tier2_oracle_harness",
    "src/backends/metal/metal_tier2.mm": "vg_metal_tier2_harness",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def cache(build: Path) -> dict[str, str]:
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def sources(build: Path, root: Path) -> Counter[tuple[str, str]]:
    result: Counter[tuple[str, str]] = Counter()
    for row in json.loads((build / "compile_commands.json").read_text()):
        command = row.get("command") or " ".join(row["arguments"])
        owner = re.search(r"CMakeFiles/([^/]+)\.dir/", command)
        require(owner is not None, f"Cannot identify Ninja target: {row['file']}")
        source = Path(row["file"])
        if not source.is_absolute():
            source = Path(row["directory"]) / source
        relative = source.resolve().relative_to(root).as_posix()
        result[(owner.group(1), relative)] += 1
    return result


def tests(build: Path) -> list[dict]:
    output = subprocess.check_output(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True
    )
    return json.loads(output)["tests"]


def check(on: Path, off: Path) -> dict:
    on_cache, off_cache = cache(on), cache(off)
    require(on_cache["BUILD_TESTING"] == "ON", "First build must enable testing")
    require(off_cache["BUILD_TESTING"] == "OFF", "Second build must disable testing")
    for key in ("CMAKE_HOME_DIRECTORY", "CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER",
                "VG_ENABLE_METAL", "VG_ENABLE_VULKAN", "VG_ENABLE_SANITIZERS"):
        require(on_cache.get(key) == off_cache.get(key), f"Build profiles differ: {key}")
    root = Path(on_cache["CMAKE_HOME_DIRECTORY"]).resolve()
    on_sources, off_sources = sources(on, root), sources(off, root)
    for stage in STAGES:
        for inventory in (on_sources, off_sources):
            owners = [(target, count) for (target, source), count in inventory.items()
                      if source == stage]
            require(owners == [("vg_backend_reference", 1)], f"Wrong stage ownership: {stage}: {owners}")
    for source, target in HARNESSES.items():
        enabled = "metal" not in source or on_cache.get("VG_ENABLE_METAL") == "ON"
        owners = [(owner, count) for (owner, path), count in on_sources.items() if path == source]
        require(owners == ([(target, 1)] if enabled else []), f"Wrong harness ownership: {source}")
        require(not any(path == source for _, path in off_sources), f"OFF compiles harness: {source}")
    def production(inventory: Counter) -> Counter:
        return Counter({key: count for key, count in inventory.items()
                        if key[1].startswith("src/") and key[1] not in HARNESSES})

    require(production(on_sources) == production(off_sources), "ON/OFF production source inventories differ")
    # G6 closes the old capture-view exception: all product tools must be
    # available independently of test registration.
    tool_delta = Counter({key: count for key, count in (on_sources - off_sources).items()
                          if key[1].startswith("tools/")})
    require(not tool_delta, f"Unexpected test-only tools: {tool_delta}")
    capture_tool = ("vg-capture-view", "tools/vg-capture-view/vg_capture_view.cpp")
    require(on_sources[capture_tool] == off_sources[capture_tool] == 1,
            "capture-view must have one product owner in both profiles")
    require(not any(path.startswith("tools/") for _, path in off_sources - on_sources),
            "Unexpected OFF-only tools")
    require(not any(path.startswith("tests/") for _, path in off_sources), "OFF compiles test sources")
    for build in (on, off):
        for name in ("reference", "metal", "vulkan"):
            if name != "reference" and on_cache.get(f"VG_ENABLE_{name.upper()}") != "ON":
                continue
            members = subprocess.check_output(["ar", "-t", str(build / f"libvg_backend_{name}.a")], text=True)
            require("tier2" not in members, f"Production archive contains Tier2: {name}")
    on_tests = tests(on)
    require(bool(on_tests), "ON has no tests")
    require(not tests(off), "OFF registers tests; use a fresh OFF build tree")
    mapping = [{key: row.get(key) for key in ("name", "command", "properties")}
               for row in on_tests]
    normalized = json.dumps(mapping, sort_keys=True, separators=(",", ":"))
    normalized = normalized.replace(str(on), "<BUILD>").replace(str(root), "<ROOT>")
    return {"testing_on": str(on), "testing_off": str(off),
            "production_compile_units": sum(production(off_sources).values()),
            "tests": len(on_tests), "off_tests": 0,
            "test_only_tools": [],
            "ctest_mapping_sha256": hashlib.sha256(normalized.encode()).hexdigest(),
            "status": "passed"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("testing_on", type=Path)
    parser.add_argument("testing_off", type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.testing_on.resolve(), args.testing_off.resolve()), indent=2))


if __name__ == "__main__":
    main()
