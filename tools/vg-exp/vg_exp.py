#!/usr/bin/env python3
"""Phase 0 evidence runner. It intentionally uses only the Python standard library."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import platform
import secrets
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DEFINITION = ROOT / "experiments" / "definitions" / "P000-phase0-probe.json"
PHASE_A_TESTS = {
    "E001": "model.phase-a",
    "E003": "model.phase-a",
    "E006": "conformance.phase-a",
    "E015": "conformance.phase-a",
    "E018": "core.unit",
}
# Phase B milestones (E002/E004/E007/E009/E012, per TASK-B10's approved plan)
# add further E*.json definitions alongside Phase A's; the Phase A runner
# only ever looks at the five ids it already knows about.
PHASE_A_DEFINITIONS = sorted(
    path for path in (ROOT / "experiments" / "definitions").glob("E*.json")
    if json.loads(path.read_text(encoding="utf-8")).get("id") in PHASE_A_TESTS
)

# Phase B gate experiments (TASK-B10 through TASK-B17). Each id maps to the
# ctest names that must all pass for that experiment to count as a real
# Metal+reference result -- not a 1:1 mapping like PHASE_A_TESTS, since some
# experiments (E009) span two ctest targets (indirect dispatch + a separate
# cull/compact kernel) and E002 additionally re-verifies the golden fixture
# suite it extended. Vulkan is never run here: per ADR-024, Vulkan evidence
# for these five experiments is compile-review-only (documentation-only
# source edits, no build target on this machine), so it is reported as a
# fixed "compile-review-only" sample rather than executed.
PHASE_B_EXPERIMENTS = {
    "E002": ["vertical-slice.metal.pointer-graph", "compiler.compute-package-golden"],
    "E004": ["vertical-slice.metal.access-certificate"],
    "E007": ["vertical-slice.metal.indexed-binding"],
    "E009": ["vertical-slice.metal.tier1-indirect", "vertical-slice.metal.cull-compact"],
    "E012": ["vertical-slice.metal.effect-dag"],
}
PHASE_B_DEFINITIONS = sorted(
    path for path in (ROOT / "experiments" / "definitions").glob("E*.json")
    if json.loads(path.read_text(encoding="utf-8")).get("id") in PHASE_B_EXPERIMENTS
)

# Phase C Representation gate (ADR-030): E005/E008/E016 decide closure;
# E013 is implemented and run but is not a hard gate row. Vulkan remains
# compile-review-only on this host (same shape as PHASE_B_EXPERIMENTS).
PHASE_C_EXPERIMENTS = {
    "E005": ["vertical-slice.metal.consume-input"],
    "E008": ["vertical-slice.metal.sample-facet"],
    "E013": ["vertical-slice.metal.pipeline-classification"],
    "E016": ["vertical-slice.metal.representation-churn"],
}
PHASE_C_GATE_EXPERIMENTS = ("E005", "E008", "E016")
PHASE_C_DEFINITIONS = sorted(
    path for path in (ROOT / "experiments" / "definitions").glob("E*.json")
    if json.loads(path.read_text(encoding="utf-8")).get("id") in PHASE_C_EXPERIMENTS
)

# Phase D Dynamic Graph / Residency (ADR-035 / ADR-041). Two JSON files
# share id E004; this mapping loads only the D2 revisit, never the B-era
# E004-access-certificate.json. Vulkan is compile-review-only (ADR-024).
PHASE_D_EXPERIMENTS: dict[str, list[dict[str, str]]] = {
    "E004": [
        {"ctest": "core.discovery", "backend": "cpu-reference"},
        {"ctest": "vertical-slice.metal.discovery", "backend": "metal"},
    ],
    "E010": [
        {"ctest": "unit.tier2-oracle", "backend": "cpu-reference"},
        {"ctest": "vertical-slice.metal.tier2-nodes", "backend": "metal"},
    ],
    "E011": [
        {"ctest": "core.working-set", "backend": "cpu-reference"},
        {"ctest": "vertical-slice.metal.working-set", "backend": "metal"},
    ],
    "E014": [
        {"ctest": "capture.view", "backend": "cpu-reference"},
        {"ctest": "capture.view.cli", "backend": "cpu-reference"},
    ],
    "E017": [
        {"ctest": "core.envelope-continuation", "backend": "cpu-reference"},
        {"ctest": "vertical-slice.metal.envelope-continuation", "backend": "metal"},
    ],
}
PHASE_D_DEFINITION_FILES = {
    "E004": "E004-discovery-revisit.json",
    "E010": "E010-heterogeneous-node-lowering.json",
    "E011": "E011-residency-working-set.json",
    "E014": "E014-capture-replay.json",
    "E017": "E017-envelope-quota-continuation.json",
}
PHASE_D_GATE_EXPERIMENTS = ("E004", "E010", "E011", "E014", "E017")


def canonical_json(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True) + "\n"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def git_identity() -> dict[str, str]:
    def git(*args: str) -> str | None:
        completed = subprocess.run(["git", *args], cwd=ROOT, text=True, capture_output=True)
        return completed.stdout.strip() if completed.returncode == 0 else None

    commit = git("rev-parse", "HEAD")
    status = git("status", "--porcelain=v1")
    diff = git("diff", "--no-ext-diff", "--binary") or ""
    return {
        "commit": commit or "unavailable",
        "dirty": "true" if status else "false",
        "dirty_diff_hash": f"sha256:{hashlib.sha256(diff.encode()).hexdigest()}",
    }


def safe_machine_id() -> str:
    raw = f"{platform.node()}:{platform.machine()}:{platform.system()}"
    return hashlib.sha256(raw.encode()).hexdigest()[:16]


def validate_definition(value: Any) -> None:
    required = ("schema", "id", "name", "workload", "backends", "metrics")
    if not isinstance(value, dict) or value.get("schema") != "vg.experiment/v1":
        raise ValueError("definition must declare schema vg.experiment/v1")
    missing = [key for key in required if key not in value]
    if missing or not isinstance(value["backends"], list) or not isinstance(value["metrics"], list):
        raise ValueError(f"invalid experiment definition; missing or invalid fields: {', '.join(missing)}")


def load_phase_a_definitions() -> list[dict[str, Any]]:
    definitions = [json.loads(path.read_text(encoding="utf-8")) for path in PHASE_A_DEFINITIONS]
    for definition in definitions:
        validate_definition(definition)
        if definition["id"] not in PHASE_A_TESTS:
            raise ValueError(f"unsupported Phase A experiment id: {definition['id']}")
    if {definition["id"] for definition in definitions} != set(PHASE_A_TESTS):
        raise ValueError("Phase A definitions must cover E001, E003, E006, E015, and E018")
    return definitions


def load_phase_b_definitions() -> list[dict[str, Any]]:
    definitions = [json.loads(path.read_text(encoding="utf-8")) for path in PHASE_B_DEFINITIONS]
    for definition in definitions:
        validate_definition(definition)
        if definition["id"] not in PHASE_B_EXPERIMENTS:
            raise ValueError(f"unsupported Phase B experiment id: {definition['id']}")
    if {definition["id"] for definition in definitions} != set(PHASE_B_EXPERIMENTS):
        raise ValueError("Phase B definitions must cover E002, E004, E007, E009, and E012")
    return definitions


def load_phase_c_definitions() -> list[dict[str, Any]]:
    definitions = [json.loads(path.read_text(encoding="utf-8")) for path in PHASE_C_DEFINITIONS]
    for definition in definitions:
        validate_definition(definition)
        if definition["id"] not in PHASE_C_EXPERIMENTS:
            raise ValueError(f"unsupported Phase C experiment id: {definition['id']}")
    if {definition["id"] for definition in definitions} != set(PHASE_C_EXPERIMENTS):
        raise ValueError("Phase C definitions must cover E005, E008, E013, and E016")
    return definitions


def load_phase_d_definitions() -> list[dict[str, Any]]:
    definitions: list[dict[str, Any]] = []
    for experiment_id in PHASE_D_GATE_EXPERIMENTS:
        path = ROOT / "experiments" / "definitions" / PHASE_D_DEFINITION_FILES[experiment_id]
        definition = json.loads(path.read_text(encoding="utf-8"))
        validate_definition(definition)
        if definition["id"] != experiment_id:
            raise ValueError(f"{path.name} must declare id {experiment_id}")
        definitions.append(definition)
    return definitions


def ctest_status(returncode: int, stdout: str, stderr: str) -> str:
    combined = f"{stdout}\n{stderr}"
    if "No tests were found" in combined:
        return "missing"
    return "passed" if returncode == 0 else "failed"


def find_probe(build_dir: Path) -> Path:
    candidates = [build_dir / "vg-platform-probe", build_dir / "Debug" / "vg-platform-probe"]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise FileNotFoundError(f"vg-platform-probe not found in {build_dir}; build the selected CMake preset first")


def create_run(definition: dict[str, Any], build_dir: Path) -> Path:
    probe = find_probe(build_dir)
    now = dt.datetime.now(dt.timezone.utc)
    run_id = f"{now:%Y%m%dT%H%M%SZ}-{definition['id']}-{git_identity()['commit'][:12]}-{safe_machine_id()}-{secrets.token_hex(4)}"
    run_dir = ROOT / "artifacts" / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    (run_dir / "lowering").mkdir()
    (run_dir / "captures").mkdir()
    (run_dir / "traces").mkdir()
    (run_dir / "outputs").mkdir()

    completed = subprocess.run([str(probe)], cwd=ROOT, text=True, capture_output=True)
    (run_dir / "stdout.log").write_text(completed.stdout, encoding="utf-8")
    (run_dir / "stderr.log").write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(f"platform probe failed with exit code {completed.returncode}")
    platform_data = json.loads(completed.stdout)
    if platform_data.get("schema") != "vg.platform/v1":
        raise RuntimeError("platform probe did not produce vg.platform/v1 JSON")

    environment = {
        "schema": "vg.environment/v1",
        "utc": now.isoformat(),
        "timezone": "UTC",
        "machine_id": safe_machine_id(),
        "os": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version.split()[0],
        "host_name": "redacted",
        "git": git_identity(),
        "capability_snapshot": platform_data,
    }
    build = {"schema": "vg.build/v1", "build_dir": build_dir.name, "probe": probe.name}
    sample = {
        "schema": "vg.sample/v1", "run_id": run_id, "experiment": definition["id"],
        "backend": "platform", "variant": "probe", "parameters": {}, "phase": "probe",
        "batch": 0, "iteration": 0, "status": "ok",
        "metrics": {"adapter_count": len(platform_data["adapters"])}, "output_hash": None,
        "lowering_report": None,
    }
    summary = {"schema": "vg.summary/v1", "run_id": run_id, "status": "ok", "adapter_count": len(platform_data["adapters"])}
    (run_dir / "environment.json").write_text(canonical_json(environment), encoding="utf-8")
    (run_dir / "build.json").write_text(canonical_json(build), encoding="utf-8")
    (run_dir / "definition.resolved.json").write_text(canonical_json(definition), encoding="utf-8")
    (run_dir / "samples.jsonl").write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")
    (run_dir / "summary.json").write_text(canonical_json(summary), encoding="utf-8")
    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=["schema", "run_id", "status", "adapter_count"])
        writer.writeheader(); writer.writerow(summary)
    (run_dir / "report.md").write_text(
        f"# {definition['name']}\n\nStatus: ok\n\nAdapters observed: {len(platform_data['adapters'])}\n"
        "\nThis bundle records platform capability evidence only; it makes no performance claim.\n", encoding="utf-8")
    write_manifest(run_dir, run_id)
    return run_dir


def write_manifest(run_dir: Path, run_id: str) -> None:
    files = {str(path.relative_to(run_dir)): sha256_file(path) for path in sorted(run_dir.rglob("*"))
             if path.is_file() and path.name != "manifest.json"}
    manifest = {"schema": "vg.run/v1", "run_id": run_id, "files": files,
                "manifest_excludes_self": True}
    (run_dir / "manifest.json").write_text(canonical_json(manifest), encoding="utf-8")


def command_probe(args: argparse.Namespace) -> int:
    definition = json.loads(DEFAULT_DEFINITION.read_text(encoding="utf-8"))
    validate_definition(definition)
    run_dir = create_run(definition, Path(args.build_dir).resolve())
    print(run_dir.relative_to(ROOT))
    return 0


def command_run(args: argparse.Namespace) -> int:
    definition = json.loads(Path(args.definition).read_text(encoding="utf-8"))
    validate_definition(definition)
    run_dir = create_run(definition, Path(args.build_dir).resolve())
    print(run_dir.relative_to(ROOT))
    return 0


def command_analyze(args: argparse.Namespace) -> int:
    run_dir = Path(args.run_dir).resolve()
    samples = [json.loads(line) for line in (run_dir / "samples.jsonl").read_text(encoding="utf-8").splitlines() if line]
    summary = {"schema": "vg.summary/v1", "run_id": samples[0]["run_id"], "sample_count": len(samples),
               "ok_count": sum(sample["status"] == "ok" for sample in samples)}
    (run_dir / "summary.json").write_text(canonical_json(summary), encoding="utf-8")
    write_manifest(run_dir, samples[0]["run_id"])
    print(run_dir / "summary.json")
    return 0


def create_phase_a_run(build_dir: Path) -> Path:
    definitions = load_phase_a_definitions()
    now = dt.datetime.now(dt.timezone.utc)
    run_id = f"{now:%Y%m%dT%H%M%SZ}-PHASEA-{git_identity()['commit'][:12]}-{safe_machine_id()}-{secrets.token_hex(4)}"
    run_dir = ROOT / "artifacts" / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    for name in ("lowering", "captures", "traces", "outputs"):
        (run_dir / name).mkdir()
    samples: list[dict[str, Any]] = []
    logs: list[str] = []
    for definition in definitions:
        test_name = PHASE_A_TESTS[definition["id"]]
        completed = subprocess.run(["ctest", "--test-dir", str(build_dir), "-R", f"^{test_name}$", "--output-on-failure"], cwd=ROOT, text=True, capture_output=True)
        logs.append(f"[{definition['id']}] ctest -R {test_name}\n{completed.stdout}\n{completed.stderr}")
        status = "passed" if completed.returncode == 0 else "failed"
        samples.append({"schema": "vg.sample/v1", "run_id": run_id, "experiment": definition["id"], "backend": "cpu-reference", "variant": "reference-conformance", "parameters": {"ctest": test_name}, "phase": "phase-a", "batch": 0, "iteration": 0, "status": status, "metrics": {"return_code": completed.returncode}, "output_hash": None, "lowering_report": None})
    environment = {"schema": "vg.environment/v1", "utc": now.isoformat(), "timezone": "UTC", "machine_id": safe_machine_id(), "os": platform.platform(), "machine": platform.machine(), "python": sys.version.split()[0], "host_name": "redacted", "git": git_identity(), "capability_snapshot": None}
    build = {"schema": "vg.build/v1", "build_dir": build_dir.name, "runner": "phase-a"}
    (run_dir / "environment.json").write_text(canonical_json(environment), encoding="utf-8")
    (run_dir / "build.json").write_text(canonical_json(build), encoding="utf-8")
    (run_dir / "definition.resolved.json").write_text(canonical_json({"schema": "vg.phase-a/v1", "experiments": definitions}), encoding="utf-8")
    (run_dir / "stdout.log").write_text("\n".join(logs), encoding="utf-8")
    (run_dir / "stderr.log").write_text("", encoding="utf-8")
    (run_dir / "samples.jsonl").write_text("".join(json.dumps(sample, sort_keys=True) + "\n" for sample in samples), encoding="utf-8")
    passed = sum(sample["status"] == "passed" for sample in samples)
    summary = {"schema": "vg.summary/v1", "run_id": run_id, "status": "ok" if passed == len(samples) else "failed", "experiment_count": len(samples), "passed": passed, "failed": len(samples) - passed, "native_adapter": "deferred"}
    (run_dir / "summary.json").write_text(canonical_json(summary), encoding="utf-8")
    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=["experiment", "status", "return_code"]); writer.writeheader()
        for sample in samples: writer.writerow({"experiment": sample["experiment"], "status": sample["status"], "return_code": sample["metrics"]["return_code"]})
    (run_dir / "report.md").write_text("# Phase A Semantic Core\n\n" + "\n".join(f"- {sample['experiment']}: {sample['status']}" for sample in samples) + "\n\nNativeAdapter results are deferred to Phase B.\n", encoding="utf-8")
    write_manifest(run_dir, run_id)
    return run_dir


def command_phase_a(args: argparse.Namespace) -> int:
    run_dir = create_phase_a_run(Path(args.build_dir).resolve())
    print(run_dir.relative_to(ROOT))
    return 0


def create_phase_b_run(build_dir: Path) -> Path:
    definitions = load_phase_b_definitions()
    now = dt.datetime.now(dt.timezone.utc)
    run_id = f"{now:%Y%m%dT%H%M%SZ}-PHASEB-{git_identity()['commit'][:12]}-{safe_machine_id()}-{secrets.token_hex(4)}"
    run_dir = ROOT / "artifacts" / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    for name in ("lowering", "captures", "traces", "outputs"):
        (run_dir / name).mkdir()
    samples: list[dict[str, Any]] = []
    logs: list[str] = []
    for definition in definitions:
        experiment_id = definition["id"]
        test_names = PHASE_B_EXPERIMENTS[experiment_id]
        experiment_ok = True
        for test_name in test_names:
            completed = subprocess.run(["ctest", "--test-dir", str(build_dir), "-R", f"^{test_name}$", "--output-on-failure"], cwd=ROOT, text=True, capture_output=True)
            logs.append(f"[{experiment_id}] ctest -R {test_name}\n{completed.stdout}\n{completed.stderr}")
            status = "passed" if completed.returncode == 0 else "failed"
            experiment_ok = experiment_ok and completed.returncode == 0
            samples.append({"schema": "vg.sample/v1", "run_id": run_id, "experiment": experiment_id, "backend": "metal", "variant": test_name, "parameters": {"ctest": test_name}, "phase": "phase-b", "batch": 0, "iteration": 0, "status": status, "metrics": {"return_code": completed.returncode}, "output_hash": None, "lowering_report": None})
        # Vulkan is never executed for these five experiments (ADR-024): the
        # source-level mapping is documentation-only and vg_backend_vulkan is
        # not a build target on this machine. Recorded as its own sample so
        # `experiment_ok` above (which only ever reflects the metal/reference
        # ctests) is never conflated with this fixed, non-executed status.
        samples.append({"schema": "vg.sample/v1", "run_id": run_id, "experiment": experiment_id, "backend": "vulkan", "variant": "compile-review-only", "parameters": {}, "phase": "phase-b", "batch": 0, "iteration": 0, "status": "compile-review-only", "metrics": {}, "output_hash": None, "lowering_report": None})
    environment = {"schema": "vg.environment/v1", "utc": now.isoformat(), "timezone": "UTC", "machine_id": safe_machine_id(), "os": platform.platform(), "machine": platform.machine(), "python": sys.version.split()[0], "host_name": "redacted", "git": git_identity(), "capability_snapshot": None}
    build = {"schema": "vg.build/v1", "build_dir": build_dir.name, "runner": "phase-b"}
    (run_dir / "environment.json").write_text(canonical_json(environment), encoding="utf-8")
    (run_dir / "build.json").write_text(canonical_json(build), encoding="utf-8")
    (run_dir / "definition.resolved.json").write_text(canonical_json({"schema": "vg.phase-b/v1", "experiments": definitions}), encoding="utf-8")
    (run_dir / "stdout.log").write_text("\n".join(logs), encoding="utf-8")
    (run_dir / "stderr.log").write_text("", encoding="utf-8")
    (run_dir / "samples.jsonl").write_text("".join(json.dumps(sample, sort_keys=True) + "\n" for sample in samples), encoding="utf-8")
    executed = [sample for sample in samples if sample["backend"] != "vulkan"]
    passed = sum(sample["status"] == "passed" for sample in executed)
    summary = {"schema": "vg.summary/v1", "run_id": run_id, "status": "ok" if passed == len(executed) else "failed", "experiment_count": len(definitions), "ctest_count": len(executed), "passed": passed, "failed": len(executed) - passed, "vulkan_status": "compile-review-only"}
    (run_dir / "summary.json").write_text(canonical_json(summary), encoding="utf-8")
    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=["experiment", "backend", "variant", "status", "return_code"]); writer.writeheader()
        for sample in samples: writer.writerow({"experiment": sample["experiment"], "backend": sample["backend"], "variant": sample["variant"], "status": sample["status"], "return_code": sample["metrics"].get("return_code")})
    (run_dir / "report.md").write_text("# Phase B Gate Experiments\n\n" + "\n".join(f"- {sample['experiment']} [{sample['backend']}/{sample['variant']}]: {sample['status']}" for sample in samples) + "\n\nVulkan samples are compile-review-only (ADR-024): documentation-level source mapping, not executed evidence.\n", encoding="utf-8")
    write_manifest(run_dir, run_id)
    return run_dir


def command_phase_b(args: argparse.Namespace) -> int:
    run_dir = create_phase_b_run(Path(args.build_dir).resolve())
    print(run_dir.relative_to(ROOT))
    return 0


def create_phase_c_run(build_dir: Path) -> Path:
    definitions = load_phase_c_definitions()
    now = dt.datetime.now(dt.timezone.utc)
    run_id = f"{now:%Y%m%dT%H%M%SZ}-PHASEC-{git_identity()['commit'][:12]}-{safe_machine_id()}-{secrets.token_hex(4)}"
    run_dir = ROOT / "artifacts" / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    for name in ("lowering", "captures", "traces", "outputs"):
        (run_dir / name).mkdir()
    samples: list[dict[str, Any]] = []
    logs: list[str] = []
    for definition in definitions:
        experiment_id = definition["id"]
        test_names = PHASE_C_EXPERIMENTS[experiment_id]
        for test_name in test_names:
            completed = subprocess.run(
                ["ctest", "--test-dir", str(build_dir), "-R", f"^{test_name}$", "--output-on-failure"],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            logs.append(f"[{experiment_id}] ctest -R {test_name}\n{completed.stdout}\n{completed.stderr}")
            status = "passed" if completed.returncode == 0 else "failed"
            samples.append(
                {
                    "schema": "vg.sample/v1",
                    "run_id": run_id,
                    "experiment": experiment_id,
                    "backend": "metal",
                    "variant": test_name,
                    "parameters": {"ctest": test_name},
                    "phase": "phase-c",
                    "batch": 0,
                    "iteration": 0,
                    "status": status,
                    "metrics": {"return_code": completed.returncode},
                    "output_hash": None,
                    "lowering_report": None,
                }
            )
        samples.append(
            {
                "schema": "vg.sample/v1",
                "run_id": run_id,
                "experiment": experiment_id,
                "backend": "vulkan",
                "variant": "compile-review-only",
                "parameters": {},
                "phase": "phase-c",
                "batch": 0,
                "iteration": 0,
                "status": "compile-review-only",
                "metrics": {},
                "output_hash": None,
                "lowering_report": None,
            }
        )
    environment = {
        "schema": "vg.environment/v1",
        "utc": now.isoformat(),
        "timezone": "UTC",
        "machine_id": safe_machine_id(),
        "os": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version.split()[0],
        "host_name": "redacted",
        "git": git_identity(),
        "capability_snapshot": None,
    }
    build = {"schema": "vg.build/v1", "build_dir": build_dir.name, "runner": "phase-c"}
    (run_dir / "environment.json").write_text(canonical_json(environment), encoding="utf-8")
    (run_dir / "build.json").write_text(canonical_json(build), encoding="utf-8")
    (run_dir / "definition.resolved.json").write_text(
        canonical_json({"schema": "vg.phase-c/v1", "experiments": definitions, "gate_experiments": list(PHASE_C_GATE_EXPERIMENTS)}),
        encoding="utf-8",
    )
    (run_dir / "stdout.log").write_text("\n".join(logs), encoding="utf-8")
    (run_dir / "stderr.log").write_text("", encoding="utf-8")
    (run_dir / "samples.jsonl").write_text(
        "".join(json.dumps(sample, sort_keys=True) + "\n" for sample in samples), encoding="utf-8"
    )
    executed = [sample for sample in samples if sample["backend"] != "vulkan"]
    passed = sum(sample["status"] == "passed" for sample in executed)
    gate_executed = [sample for sample in executed if sample["experiment"] in PHASE_C_GATE_EXPERIMENTS]
    gate_passed = sum(sample["status"] == "passed" for sample in gate_executed)
    summary = {
        "schema": "vg.summary/v1",
        "run_id": run_id,
        "status": "ok" if passed == len(executed) else "failed",
        "experiment_count": len(definitions),
        "ctest_count": len(executed),
        "passed": passed,
        "failed": len(executed) - passed,
        "vulkan_status": "compile-review-only",
        "gate_experiments": list(PHASE_C_GATE_EXPERIMENTS),
        "gate_passed": gate_passed,
        "gate_failed": len(gate_executed) - gate_passed,
        "e013_non_blocking": True,
    }
    (run_dir / "summary.json").write_text(canonical_json(summary), encoding="utf-8")
    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=["experiment", "backend", "variant", "status", "return_code"]
        )
        writer.writeheader()
        for sample in samples:
            writer.writerow(
                {
                    "experiment": sample["experiment"],
                    "backend": sample["backend"],
                    "variant": sample["variant"],
                    "status": sample["status"],
                    "return_code": sample["metrics"].get("return_code"),
                }
            )
    (run_dir / "report.md").write_text(
        "# Phase C Gate Experiments\n\n"
        + "\n".join(
            f"- {sample['experiment']} [{sample['backend']}/{sample['variant']}]: {sample['status']}"
            for sample in samples
        )
        + "\n\nGate closure uses E005/E008/E016 only (ADR-030); E013 is recorded but non-blocking.\n"
        + "Vulkan samples are compile-review-only (ADR-024/030): documentation-level source mapping, not executed evidence.\n",
        encoding="utf-8",
    )
    write_manifest(run_dir, run_id)
    return run_dir


def command_phase_c(args: argparse.Namespace) -> int:
    run_dir = create_phase_c_run(Path(args.build_dir).resolve())
    print(run_dir.relative_to(ROOT))
    return 0


def create_phase_d_run(build_dir: Path) -> Path:
    definitions = load_phase_d_definitions()
    now = dt.datetime.now(dt.timezone.utc)
    run_id = f"{now:%Y%m%dT%H%M%SZ}-PHASED-{git_identity()['commit'][:12]}-{safe_machine_id()}-{secrets.token_hex(4)}"
    run_dir = ROOT / "artifacts" / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    for name in ("lowering", "captures", "traces", "outputs"):
        (run_dir / name).mkdir()
    samples: list[dict[str, Any]] = []
    logs: list[str] = []
    for definition in definitions:
        experiment_id = definition["id"]
        for row in PHASE_D_EXPERIMENTS[experiment_id]:
            test_name = row["ctest"]
            completed = subprocess.run(
                ["ctest", "--test-dir", str(build_dir), "-R", f"^{test_name}$", "--output-on-failure"],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            logs.append(f"[{experiment_id}] ctest -R {test_name}\n{completed.stdout}\n{completed.stderr}")
            status = ctest_status(completed.returncode, completed.stdout, completed.stderr)
            samples.append(
                {
                    "schema": "vg.sample/v1",
                    "run_id": run_id,
                    "experiment": experiment_id,
                    "backend": row["backend"],
                    "variant": test_name,
                    "parameters": {"ctest": test_name},
                    "phase": "phase-d",
                    "batch": 0,
                    "iteration": 0,
                    "status": status,
                    "metrics": {"return_code": completed.returncode},
                    "output_hash": None,
                    "lowering_report": definition.get("phase_d_classification"),
                }
            )
        samples.append(
            {
                "schema": "vg.sample/v1",
                "run_id": run_id,
                "experiment": experiment_id,
                "backend": "vulkan",
                "variant": "compile-review-only",
                "parameters": {},
                "phase": "phase-d",
                "batch": 0,
                "iteration": 0,
                "status": "compile-review-only",
                "metrics": {},
                "output_hash": None,
                "lowering_report": None,
            }
        )
    environment = {
        "schema": "vg.environment/v1",
        "utc": now.isoformat(),
        "timezone": "UTC",
        "machine_id": safe_machine_id(),
        "os": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version.split()[0],
        "host_name": "redacted",
        "git": git_identity(),
        "capability_snapshot": None,
    }
    build = {"schema": "vg.build/v1", "build_dir": build_dir.name, "runner": "phase-d"}
    (run_dir / "environment.json").write_text(canonical_json(environment), encoding="utf-8")
    (run_dir / "build.json").write_text(canonical_json(build), encoding="utf-8")
    (run_dir / "definition.resolved.json").write_text(
        canonical_json(
            {
                "schema": "vg.phase-d/v1",
                "experiments": definitions,
                "gate_experiments": list(PHASE_D_GATE_EXPERIMENTS),
                "definition_files": PHASE_D_DEFINITION_FILES,
                "reports": [
                    "docs/reports/phase-d-gate.md",
                    "docs/reports/host-assisted-boundary.md",
                    "docs/reports/native-contract-research-v1.md",
                ],
            }
        ),
        encoding="utf-8",
    )
    (run_dir / "stdout.log").write_text("\n".join(logs), encoding="utf-8")
    (run_dir / "stderr.log").write_text("", encoding="utf-8")
    (run_dir / "samples.jsonl").write_text(
        "".join(json.dumps(sample, sort_keys=True) + "\n" for sample in samples), encoding="utf-8"
    )
    executed = [sample for sample in samples if sample["backend"] != "vulkan"]
    passed = sum(sample["status"] == "passed" for sample in executed)
    summary = {
        "schema": "vg.summary/v1",
        "run_id": run_id,
        "status": "ok" if passed == len(executed) else "failed",
        "experiment_count": len(definitions),
        "ctest_count": len(executed),
        "passed": passed,
        "failed": len(executed) - passed,
        "vulkan_status": "compile-review-only",
        "gate_experiments": list(PHASE_D_GATE_EXPERIMENTS),
        "break_even_curves": "unmeasured",
        "e004_historical_b_row": "experiments/definitions/E004-access-certificate.json",
    }
    (run_dir / "summary.json").write_text(canonical_json(summary), encoding="utf-8")
    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=["experiment", "backend", "variant", "status", "return_code"]
        )
        writer.writeheader()
        for sample in samples:
            writer.writerow(
                {
                    "experiment": sample["experiment"],
                    "backend": sample["backend"],
                    "variant": sample["variant"],
                    "status": sample["status"],
                    "return_code": sample["metrics"].get("return_code"),
                }
            )
    (run_dir / "report.md").write_text(
        "# Phase D Gate Experiments\n\n"
        + "\n".join(
            f"- {sample['experiment']} [{sample['backend']}/{sample['variant']}]: {sample['status']}"
            for sample in samples
        )
        + "\n\nE004 uses E004-discovery-revisit.json; the B-era E004-access-certificate.json row is historical (ADR-025/035).\n"
        + "Vulkan samples are compile-review-only (ADR-024/035/041).\n"
        + "Break-even curves are unmeasured (sample insufficient); see docs/reports/phase-d-gate.md.\n"
        + "HostAssisted boundary: docs/reports/host-assisted-boundary.md.\n"
        + "NativeContractResearch v1: docs/reports/native-contract-research-v1.md.\n",
        encoding="utf-8",
    )
    write_manifest(run_dir, run_id)
    return run_dir


def command_phase_d(args: argparse.Namespace) -> int:
    run_dir = create_phase_d_run(Path(args.build_dir).resolve())
    print(run_dir.relative_to(ROOT))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="VG Phase 0 experiment runner")
    subparsers = parser.add_subparsers(dest="command", required=True)
    probe = subparsers.add_parser("probe"); probe.add_argument("--build-dir", required=True); probe.set_defaults(func=command_probe)
    run = subparsers.add_parser("run"); run.add_argument("definition"); run.add_argument("--build-dir", required=True); run.set_defaults(func=command_run)
    analyze = subparsers.add_parser("analyze"); analyze.add_argument("run_dir"); analyze.set_defaults(func=command_analyze)
    phase_a = subparsers.add_parser("phase-a"); phase_a.add_argument("--build-dir", required=True); phase_a.set_defaults(func=command_phase_a)
    phase_b = subparsers.add_parser("phase-b"); phase_b.add_argument("--build-dir", required=True); phase_b.set_defaults(func=command_phase_b)
    phase_c = subparsers.add_parser("phase-c"); phase_c.add_argument("--build-dir", required=True); phase_c.set_defaults(func=command_phase_c)
    phase_d = subparsers.add_parser("phase-d"); phase_d.add_argument("--build-dir", required=True); phase_d.set_defaults(func=command_phase_d)
    try:
        args = parser.parse_args()
        return args.func(args)
    except (FileNotFoundError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"vg-exp: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
