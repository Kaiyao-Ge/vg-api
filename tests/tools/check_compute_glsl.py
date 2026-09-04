#!/usr/bin/env python3
"""Compile generated compute GLSL without loading a Vulkan driver or creating a GPU.

Golden comparison alone cannot detect syntactically invalid shaders. Use the
same glslc stage/target flags as the production Vulkan pipeline compiler.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


def check(root, emitter, glslc):
    names = ("load_only", "store_only", "atomic_add_only", "mixed", "indexed_store")
    with tempfile.TemporaryDirectory(prefix="vg-compute-glsl-") as directory:
        subprocess.run([str(emitter), str(root), directory], check=True, timeout=60)
        for name in names:
            source = (Path(directory) / (name + ".comp")).read_bytes()
            result = subprocess.run(
                [str(glslc), "-fshader-stage=compute", "--target-env=vulkan1.2", "-o", "-", "-"],
                input=source, capture_output=True, timeout=60,
            )
            if result.returncode:
                raise RuntimeError(f"{name}: glslc failed:\n{result.stderr.decode(errors='replace')}")
            if (len(result.stdout) < 20 or len(result.stdout) % 4 != 0
                    or result.stdout[:4] != b"\x03\x02\x23\x07"):
                raise RuntimeError(f"{name}: glslc did not produce a SPIR-V module")
            print(f"{name}: GLSL -> SPIR-V passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--emitter", required=True, type=Path)
    parser.add_argument("--glslc", required=True, type=Path)
    args = parser.parse_args()
    check(args.root.resolve(), args.emitter.resolve(), args.glslc.resolve())
