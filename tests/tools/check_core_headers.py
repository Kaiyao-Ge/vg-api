#!/usr/bin/env python3
"""Explicit G1 gate: self-contained core headers and acyclic local includes.

This read-only check is intentionally not registered as a recursive CTest.
Pass the generated directory from an already configured/built CMake tree.
"""

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
OWNERS = (
    "arena", "facet", "representation", "pointer_graph", "effect_graph",
    "task_graph", "access", "node", "envelope", "execution_result",
)


def type_definitions(source):
    """Extract the frozen G1 header's top-level class/struct/enum blocks.

    This is a bounded migration check, not a general C++ parser. Mask comments
    and literals before balancing braces but compare the original bytes.
    Nested definitions remain inside their unchanged owning type.
    """
    lexical = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)
    masked = lexical.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), source)
    declaration = re.compile(r'^(?:class|struct|enum class) (\w+)[^\n{;]*\{', re.MULTILINE)
    position = 0
    while match := declaration.search(masked, position):
        cursor = match.end() - 1
        depth = 0
        while cursor < len(masked):
            if masked[cursor] == "{":
                depth += 1
            elif masked[cursor] == "}":
                depth -= 1
                if depth == 0:
                    break
            cursor += 1
        if depth != 0 or masked[cursor + 1:cursor + 2] != ";":
            raise ValueError(f"unrecognized type boundary: {match.group(1)}")
        position = cursor + 2
        yield match.group(1), source[match.start():position]


def implementation_lines(source):
    return Counter(
        line for line in source.splitlines()
        if line.strip() and not line.startswith("#include")
        and line not in ("namespace vg::core {", "}  // namespace vg::core")
    )


def migration_errors(root, baseline_ref, headers, sources):
    """Check unique unchanged type owners and exact implementation-line moves."""
    def baseline(path):
        return subprocess.run(
            ["git", "-C", str(root), "show", f"{baseline_ref}:{path}"],
            check=True, text=True, capture_output=True,
        ).stdout

    original = dict(type_definitions(baseline("src/core/core.h")))
    current = {}
    errors = []
    for owner in (*OWNERS, "resource_types"):
        for name, body in type_definitions(headers[owner + ".h"]):
            if name in current:
                errors.append(f"duplicate type owner: {name}")
            current[name] = body
    for name in original.keys() | current.keys():
        if original.get(name) != current.get(name):
            errors.append(f"changed/missing/new type definition: {name}")
    old_lines = implementation_lines(baseline("src/core/core.cpp"))
    new_lines = Counter()
    for owner in OWNERS:
        new_lines.update(implementation_lines(sources[owner + ".cpp"]))
    if old_lines != new_lines:
        errors.append("core.cpp implementation lines changed, omitted or duplicated")
    if not errors:
        print(f"PASS: {len(original)} unchanged unique type owners; {sum(old_lines.values())} implementation lines conserved against {baseline_ref}")
    return errors


def dependency_errors(headers):
    """Return dependency violations for a {core-relative name: source} map."""
    errors = []
    edges = {}
    for name, source in headers.items():
        includes = INCLUDE.findall(source)
        for include in includes:
            if name != "core.h" and include == "core/core.h":
                errors.append(f"{name}: reverse dependency on compatibility umbrella")
            if include.startswith(("backends/", "Metal/", "vulkan/")) or include in (
                "vg/vg.h", "include/vg/vg.h", "vg.h"
            ):
                errors.append(f"{name}: forbidden backend/public ABI include {include}")
        edges[name] = [
            include.removeprefix("core/") for include in includes
            if include.startswith("core/")
        ]
    done = set()

    def visit(name, stack):
        if name in stack:
            errors.append("core header include cycle: " + " -> ".join(stack + [name]))
            return
        if name in done:
            return
        if name not in edges:
            errors.append(f"missing core header: {name}")
            return
        for dependency in edges[name]:
            visit(dependency, stack + [name])
        done.add(name)

    for name in edges:
        visit(name, [])
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--baseline-ref", help="Optional pre-G1 Git ref for migration conservation checks")
    args = parser.parse_args()
    root = args.root.resolve()
    headers = {
        path.name: path.read_text() for path in sorted((root / "src/core").glob("*.h"))
    }
    errors = dependency_errors(headers)
    # Source files also must not restore the umbrella/backend/public ABI dependency.
    sources = {
        path.name: path.read_text() for path in sorted((root / "src/core").glob("*.cpp"))
    }
    if args.baseline_ref:
        try:
            errors.extend(migration_errors(root, args.baseline_ref, headers, sources))
        except (KeyError, ValueError, subprocess.CalledProcessError) as error:
            errors.append(f"migration check failed: {error}")
    for name, source in sources.items():
        for include in INCLUDE.findall(source):
            if include == "core/core.h" or include.startswith(("backends/", "Metal/", "vulkan/")) or include in (
                "vg/vg.h", "include/vg/vg.h", "vg.h"
            ):
                errors.append(f"{name}: forbidden implementation include {include}")
    if not headers:
        errors.append("no core headers found")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    command = shlex.split(args.cxx) + [
        "-std=c++20", "-I" + str(root / "src"),
        "-I" + str(args.generated_dir.resolve()), "-x", "c++", "-fsyntax-only", "-",
    ]
    for name in headers:
        # Repeated inclusion also proves each header's own guard works.
        source = f'#include "core/{name}"\n' * 2
        result = subprocess.run(command, input=source, text=True, capture_output=True)
        if result.returncode:
            print(f"{name}: standalone compilation failed\n{result.stderr}", file=sys.stderr)
            return 1
    print(f"PASS: {len(headers)} self-contained core headers; acyclic includes; no umbrella/backend/public ABI reverse dependency")
    return 0


if __name__ == "__main__":
    sys.exit(main())
