#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch


root = Path(sys.argv[1]).resolve()
build_dir = Path(sys.argv[2]).resolve()

# A probe can print valid JSON and still fail at process exit (e.g. LSan).
# Retain its diagnostics and do not publish a successful bundle in that case.
spec = importlib.util.spec_from_file_location("bundle_runner", root / "tools/vg-exp/vg_exp.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
with tempfile.TemporaryDirectory(prefix="vg-probe-failure-") as directory:
    temporary_root = Path(directory)
    failure = subprocess.CompletedProcess(["probe"], 1, '{"schema":"vg.platform/v1"}\n',
                                          "LeakSanitizer: test diagnostic\n")
    with patch.object(runner, "ROOT", temporary_root), \
         patch.object(runner, "find_probe", return_value=temporary_root / "probe"), \
         patch.object(runner, "git_identity", return_value={"commit": "a" * 40}), \
         patch.object(runner.subprocess, "run", return_value=failure):
        try:
            runner.create_run({"id": "probe-failure"}, temporary_root)
        except RuntimeError as error:
            logs = list(temporary_root.glob("artifacts/runs/*/stderr.log"))
            assert len(logs) == 1
            assert logs[0].read_text() == failure.stderr
            assert logs[0].with_name("stdout.log").read_text() == failure.stdout
            assert not logs[0].with_name("manifest.json").exists()
            assert str(logs[0].relative_to(temporary_root)) in str(error)
            assert "exit code 1" in str(error) and failure.stderr in str(error)
        else:
            raise AssertionError("failed probe incorrectly accepted as a successful run")

completed = subprocess.run(
    [sys.executable, str(root / "tools" / "vg-exp" / "vg_exp.py"), "probe", "--build-dir", str(build_dir)],
    cwd=root, text=True, capture_output=True,
)
if completed.returncode:
    print(completed.stdout, end="", file=sys.stderr)
    print(completed.stderr, end="", file=sys.stderr)
    completed.check_returncode()
run_dir = root / completed.stdout.strip()
manifest = json.loads((run_dir / "manifest.json").read_text())
required = {"environment.json", "build.json", "definition.resolved.json", "stdout.log", "stderr.log",
            "samples.jsonl", "summary.json", "summary.csv", "report.md"}
assert required <= set(manifest["files"])
assert "manifest.json" not in manifest["files"]
assert str(root) not in (run_dir / "environment.json").read_text()
for relative, expected_hash in manifest["files"].items():
    actual = "sha256:" + hashlib.sha256((run_dir / relative).read_bytes()).hexdigest()
    assert actual == expected_hash
