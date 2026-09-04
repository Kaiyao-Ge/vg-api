#!/usr/bin/env python3
"""Explicit migration check against a saved G6 baseline (not a recursive CTest).

Usage: check_g6_build.py build/g6-baseline/state.json build/g6-reference dev-reference
The last argument selects the baseline profile; run for Metal as well.
Use --incremental only on an idle build tree: it temporarily removes one
generated output per schema, verifies regeneration, then checks a no-op build.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from check_build_boundary import sources, tests


def check(baseline_path, build, profile, incremental=False):
    root = Path(__file__).resolve().parents[2]
    baseline = json.loads(baseline_path.read_text())[profile]
    old = Counter(tuple(row) for row in baseline["sources"])
    assert sources(build, root) == old, "target/source ownership changed"
    def normalized(rows, directory):
        rows = [{key: row.get(key) for key in ("name", "command", "properties")} for row in rows]
        return json.loads(json.dumps(rows).replace(str(directory), "<BUILD>"))
    current = tests(build)
    added = [row for row in current if row["name"] == "tooling.phase-runner-contract"]
    assert len(added) == 1
    assert normalized([row for row in current if row not in added], build) == normalized(
        baseline["tests"], root / "build" / profile), "original CTest contract changed"
    # Production schema outputs, including every auxiliary header.
    production_outputs = (
        "vg_task_root.h", "vg_task_root.reflection.json", "vg_task_root.layout.json",
        "vg_scene_root.h", "vg_scene_root_layout.h", "vg_scene_root_msl.h",
        "vg_scene_root.reflection.json", "vg_scene_root.layout.json",
        "vg_compute_task_ring.h", "vg_compute_task_ring_layout.h", "vg_compute_task_ring_words.h",
        "vg_compute_task_ring.reflection.json", "vg_compute_task_ring.layout.json",
    )
    for name in production_outputs:
        data = (build / "generated" / name).read_bytes()
        assert hashlib.sha256(data).hexdigest() == baseline["generated"][name], name
    print(f"{profile}: source owners identical; {len(current)-1} old CTests identical + 1 contract test; 13 schema outputs byte-identical")
    if incremental:
        def generate():
            subprocess.run(["cmake", "--build", str(build), "--target", "vg_schema_generate"], check=True)

        # Preserve each original until its replacement has been verified. These
        # paths are generated build artifacts, never tracked schemas or goldens.
        for name in ("vg_task_root.h", "vg_scene_root_msl.h", "vg_compute_task_ring_words.h"):
            output = build / "generated" / name
            with tempfile.TemporaryDirectory(prefix="g6-schema-", dir=build) as directory:
                saved = Path(directory) / name
                output.rename(saved)
                try:
                    generate()
                    assert output.read_bytes() == saved.read_bytes(), name
                except BaseException:
                    saved.replace(output)
                    raise
            for other in production_outputs:
                assert hashlib.sha256((build / "generated" / other).read_bytes()).hexdigest() == baseline["generated"][other], other
        times = {name: (build / "generated" / name).stat().st_mtime_ns for name in production_outputs}
        generate()
        assert times == {name: (build / "generated" / name).stat().st_mtime_ns for name in production_outputs}, "no-op regenerated schema outputs"
        print(f"{profile}: three single-output regeneration cases and no-op build passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("profile")
    parser.add_argument("--incremental", action="store_true")
    args = parser.parse_args()
    check(args.baseline, args.build.resolve(), args.profile, args.incremental)
