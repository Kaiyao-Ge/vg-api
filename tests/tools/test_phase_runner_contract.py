#!/usr/bin/env python3
"""Deterministic phase artifacts, evidence failures, and exact CTest routing.

The checked-in hashes came from the pre-G6 runner, not the implementation under
test. CTest is mocked here; the existing phase smoke tests exercise real CTest.
"""
from __future__ import annotations

import copy
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


def load_runner(source: str | None = None):
    path = ROOT / "tools/vg-exp/vg_exp.py"
    module = types.ModuleType("phase_contract_runner")
    module.__file__ = str(path)
    exec(compile(path.read_text() if source is None else source, str(path), "exec"), module.__dict__)
    return module


class FixedDateTime(datetime.datetime):
    @classmethod
    def now(cls, tz=None):
        return cls(2026, 9, 3, 0, 0, 0, tzinfo=datetime.timezone.utc)


def run_case(module, phase: str, outcome: str):
    calls = []
    def ctest(command, **kwargs):
        assert command[:1] == ["ctest"]
        assert command[3] == "-R" and command[5:] == ["--output-on-failure"]
        calls.append(command[4])
        failed = outcome == "failed" and len(calls) == 1
        output = {"missing": "No tests were found!!!", "skipped": "1/1 Test #1: row ... ***Skipped",
                  "not-run": "1/1 Test #1: row ... ***Not Run (Disabled)"}.get(outcome, "failure\n" if failed else "success\n")
        return subprocess.CompletedProcess(command, 8 if failed else 0, output, "")

    with tempfile.TemporaryDirectory(prefix="vg-phase-contract-") as directory:
        root = Path(directory)
        shutil.copytree(ROOT / "experiments/definitions", root / "experiments/definitions")
        with patch.object(module, "ROOT", root), \
             patch.object(module, "git_identity", return_value={"commit": "a" * 40, "dirty": "false", "dirty_diff_hash": "sha256:fixed"}), \
             patch.object(module, "safe_machine_id", return_value="machine"), \
             patch.object(module.dt, "datetime", FixedDateTime), \
             patch.object(module.secrets, "token_hex", return_value="00000000"), \
             patch.object(module.platform, "platform", return_value="test-os"), \
             patch.object(module.platform, "machine", return_value="test-cpu"), \
             patch.object(module.sys, "version", "3.10.0"), \
             patch.object(module.subprocess, "run", side_effect=ctest):
            if hasattr(module, "create_phase_run"):
                result = module.create_phase_run(phase, root / "build/contract")
            else:
                result = getattr(module, "create_" + phase.replace("-", "_") + "_run")(root / "build/contract")
        artifacts = {p.name: p.read_bytes() for p in result.iterdir() if p.is_file()}
        summary = json.loads(artifacts["summary.json"])
        samples = [json.loads(line) for line in artifacts["samples.jsonl"].splitlines()]
        manifest = json.loads(artifacts["manifest.json"])
        hashes = {name: "sha256:" + hashlib.sha256(data).hexdigest()
                  for name, data in artifacts.items() if name != "manifest.json"}
        assert manifest["files"] == hashes
        return {"artifacts": {name: hashlib.sha256(data).hexdigest() for name, data in artifacts.items()},
                "calls": calls}, summary, samples


def main():
    module = load_runner()
    expected = json.loads((ROOT / "tests/fixtures/phase-runner-contract.json").read_text())
    for phase in ("phase-a", "phase-b", "phase-c", "phase-d", "phase-e"):
        for outcome in ("passed", "failed"):
            actual, summary, samples = run_case(module, phase, outcome)
            baseline = expected[phase][outcome]
            # Routing is intentionally tightened from regex dots to literal names.
            assert actual["calls"] == ["^" + re.escape(name[1:-1]) + "$" for name in baseline["calls"]]
            assert actual["artifacts"] == baseline["artifacts"], (phase, outcome, actual, baseline)
        for outcome in ("missing", "skipped", "not-run"):
            _, summary, samples = run_case(module, phase, outcome)
            assert summary["status"] == "failed" and summary["passed"] == 0
            assert summary["failed"] > 0
            assert all(s["status"] != "passed" for s in samples)
            review = [s for s in samples if s["backend"] == "vulkan"]
            assert all(s["status"] == "compile-review-only" and s["metrics"] == {} for s in review)
    expected_vulkan = {
        "facets", "representation", "consume-input", "facet-raster",
        "pipeline-classification", "indirect", "cull-compact", "indexed-address", "tier2",
    }
    for outcome in ("passed", "failed", "missing", "skipped", "not-run"):
        actual, summary, samples = run_case(module, "vulkan-df", outcome)
        assert len(samples) == len(actual["calls"]) == 9
        assert {s["parameters"]["ctest"] for s in samples} == {
            "vertical-slice.vulkan." + name for name in expected_vulkan
        }
        assert all(s["backend"] == "vulkan" for s in samples)
        assert summary["status"] == ("ok" if outcome == "passed" else "failed")
        assert summary["performance"] == "unmeasured"
    # New required-device acceptance returns a failing process status as well
    # as preserving failed evidence; historical phase command behavior stays intact.
    with tempfile.TemporaryDirectory(prefix="vg-required-exit-") as directory:
        root = Path(directory)
        for status, expected_code in (("ok", 0), ("failed", 1)):
            (root / "summary.json").write_text(json.dumps({"status": status}))
            with patch.object(module, "ROOT", root), \
                 patch.object(module, "create_phase_run", return_value=root), \
                 patch("builtins.print"):
                assert module.command_phase(types.SimpleNamespace(command="vulkan-df", build_dir=str(root))) == expected_code
    for mutation in ("empty", "wrong-id", "unknown-gate", "fake-evidence"):
        phases = copy.deepcopy(module.PHASES)
        spec = phases["phase-a"]
        if mutation == "empty": spec["experiments"]["E001"] = []
        if mutation == "wrong-id": spec["definition_files"]["E001"] = "E003-task-publication.json"
        if mutation == "unknown-gate": spec["gate_experiments"] = ["unknown"]
        if mutation == "fake-evidence": spec["non_executed"] = [{"backend": "vulkan", "evidence": "passed"}]
        with patch.object(module, "PHASES", phases):
            try:
                module.load_phase_definitions("phase-a")
            except ValueError:
                pass
            else:
                raise AssertionError(mutation)
    print("phase contract: 10 legacy artifact cases, 15 missing/skip cases, 4 inventory negatives, 5 Vulkan D/F routing cases and required exit statuses passed")


if __name__ == "__main__":
    main()
