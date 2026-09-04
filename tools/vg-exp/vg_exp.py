#!/usr/bin/env python3
"""Evidence runner with an explicit Phase A–E catalog; standard library only."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import platform
import re
import secrets
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DEFINITION = ROOT / "experiments" / "definitions" / "P000-phase0-probe.json"
# Phase identity, sources and evidence policy are explicit data, never directory discovery.
PHASES = json.loads((Path(__file__).with_name("phase_catalog.json")).read_text(encoding="utf-8"))["phases"]
PHASE_E_BENCHMARK_CTEST = "vertical-slice.metal.tier2-nodes"
PHASE_E_BENCHMARK_EXPERIMENT = "E010"


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


def ctest_status(returncode: int, stdout: str, stderr: str) -> str:
    combined = f"{stdout}\n{stderr}"
    if "No tests were found" in combined:
        return "missing"
    if re.search(r"\*{3}Skipped|\*{3}Not Run|\(Skipped\)", combined, re.IGNORECASE):
        return "skipped"
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
        raise RuntimeError(
            f"platform probe failed with exit code {completed.returncode}; "
            f"diagnostics saved in {run_dir.relative_to(ROOT)}/stderr.log\n{completed.stderr}"
        )
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


def load_phase_definitions(phase: str) -> list[dict[str, Any]]:
    spec = PHASES[phase]
    files = spec["definition_files"]
    if not files or set(files) != set(spec["experiments"]):
        raise ValueError(f"{phase}: definition/test inventory mismatch")
    if not set(spec.get("gate_experiments", [])) <= set(files):
        raise ValueError(f"{phase}: gate refers to an unknown experiment")
    definitions = []
    for experiment_id, filename in files.items():
        if Path(filename).name != filename:
            raise ValueError(f"{phase}: definition must be a filename: {filename}")
        path = ROOT / "experiments" / "definitions" / filename
        definition = json.loads(path.read_text(encoding="utf-8"))
        validate_definition(definition)
        if definition["id"] != experiment_id:
            raise ValueError(f"{filename} must declare id {experiment_id}")
        rows = spec["experiments"][experiment_id]
        if not rows or any(not row.get("ctest") or not row.get("backend") for row in rows):
            raise ValueError(f"{phase}: no executable evidence for {experiment_id}")
        definitions.append(definition)
    for row in spec["non_executed"]:
        if row.get("evidence") != "compile-review-only" or not row.get("backend"):
            raise ValueError(f"{phase}: unsupported non-executed evidence declaration")
    return definitions


def create_phase_run(phase: str, build_dir: Path) -> Path:
    spec = PHASES[phase]
    definitions = load_phase_definitions(phase)
    now = dt.datetime.now(dt.timezone.utc)
    tag = phase.replace("-", "").upper()
    run_id = f"{now:%Y%m%dT%H%M%SZ}-{tag}-{git_identity()['commit'][:12]}-{safe_machine_id()}-{secrets.token_hex(4)}"
    run_dir = ROOT / "artifacts" / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    for name in ("lowering", "captures", "traces", "outputs"):
        (run_dir / name).mkdir()
    samples: list[dict[str, Any]] = []
    executed: list[dict[str, Any]] = []
    logs: list[str] = []
    for definition in definitions:
        experiment_id = definition["id"]
        for row in spec["experiments"][experiment_id]:
            test_name = row["ctest"]
            # Match one literal CTest name, not a regex-shaped approximation.
            completed = subprocess.run(
                ["ctest", "--test-dir", str(build_dir), "-R", f"^{re.escape(test_name)}$", "--output-on-failure"],
                cwd=ROOT, text=True, capture_output=True,
            )
            logs.append(f"[{experiment_id}] ctest -R {test_name}\n{completed.stdout}\n{completed.stderr}")
            sample = {
                "schema": "vg.sample/v1", "run_id": run_id, "experiment": experiment_id,
                "backend": row["backend"], "variant": spec.get("sample_variant", test_name),
                "parameters": {"ctest": test_name}, "phase": phase, "batch": 0, "iteration": 0,
                "status": ctest_status(completed.returncode, completed.stdout, completed.stderr),
                "metrics": {"return_code": completed.returncode}, "output_hash": None,
                "lowering_report": definition.get(spec.get("classification_field", "")),
            }
            samples.append(sample)
            executed.append(sample)
        for row in spec["non_executed"]:
            samples.append({
                "schema": "vg.sample/v1", "run_id": run_id, "experiment": experiment_id,
                "backend": row["backend"], "variant": row["evidence"], "parameters": {},
                "phase": phase, "batch": 0, "iteration": 0, "status": row["evidence"],
                "metrics": {}, "output_hash": None, "lowering_report": None,
            })
    environment = {
        "schema": "vg.environment/v1", "utc": now.isoformat(), "timezone": "UTC",
        "machine_id": safe_machine_id(), "os": platform.platform(), "machine": platform.machine(),
        "python": sys.version.split()[0], "host_name": "redacted", "git": git_identity(),
        "capability_snapshot": None,
    }
    build = {"schema": "vg.build/v1", "build_dir": build_dir.name, "runner": phase}
    resolved = {"schema": f"vg.{phase}/v1", "experiments": definitions, **spec["resolved_extra"]}
    passed = sum(sample["status"] == "passed" for sample in executed)
    summary = {
        "schema": "vg.summary/v1", "run_id": run_id,
        "status": "ok" if executed and passed == len(executed) else "failed",
        "experiment_count": len(definitions), "passed": passed, "failed": len(executed) - passed,
        **spec["summary_extra"],
    }
    if not spec.get("simple_report"):
        summary["ctest_count"] = len(executed)
    for row in spec["non_executed"]:
        summary[row["summary_key"]] = row["evidence"]
    if "gate_experiments" in spec:
        gate = spec["gate_experiments"]
        resolved["gate_experiments"] = gate
        summary["gate_experiments"] = gate
        if spec.get("gate_counts"):
            gate_samples = [sample for sample in executed if sample["experiment"] in gate]
            summary["gate_passed"] = sum(sample["status"] == "passed" for sample in gate_samples)
            summary["gate_failed"] = len(gate_samples) - summary["gate_passed"]
    for filename, data in (("environment.json", environment), ("build.json", build),
                           ("definition.resolved.json", resolved), ("summary.json", summary)):
        (run_dir / filename).write_text(canonical_json(data), encoding="utf-8")
    (run_dir / "stdout.log").write_text("\n".join(logs), encoding="utf-8")
    (run_dir / "stderr.log").write_text("", encoding="utf-8")
    (run_dir / "samples.jsonl").write_text(
        "".join(json.dumps(sample, sort_keys=True) + "\n" for sample in samples), encoding="utf-8")
    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=spec["csv_fields"])
        writer.writeheader()
        for sample in samples:
            row = {**sample, "return_code": sample["metrics"].get("return_code")}
            writer.writerow({key: row[key] for key in spec["csv_fields"]})
    lines = [f"- {s['experiment']}: {s['status']}" if spec.get("simple_report") else
             f"- {s['experiment']} [{s['backend']}/{s['variant']}]: {s['status']}" for s in samples]
    (run_dir / "report.md").write_text(
        f"# {spec['title']}\n\n" + "\n".join(lines) + spec["report_footer"], encoding="utf-8")
    write_manifest(run_dir, run_id)
    return run_dir


def command_phase(args: argparse.Namespace) -> int:
    run_dir = create_phase_run(args.command, Path(args.build_dir).resolve())
    print(run_dir.relative_to(ROOT))
    return 0


def create_benchmark_run(build_dir: Path) -> Path:
    now = dt.datetime.now(dt.timezone.utc)
    run_id = f"{now:%Y%m%dT%H%M%SZ}-BENCH-{git_identity()['commit'][:12]}-{safe_machine_id()}-{secrets.token_hex(4)}"
    run_dir = ROOT / "artifacts" / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    for name in ("lowering", "captures", "traces", "outputs"):
        (run_dir / name).mkdir()
    started = time.perf_counter()
    completed = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "-R", f"^{PHASE_E_BENCHMARK_CTEST}$", "--output-on-failure"],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )
    host_s = time.perf_counter() - started
    status = ctest_status(completed.returncode, completed.stdout, completed.stderr)
    sample = {
        "schema": "vg.sample/v1",
        "run_id": run_id,
        "experiment": PHASE_E_BENCHMARK_EXPERIMENT,
        "backend": "metal",
        "variant": PHASE_E_BENCHMARK_CTEST,
        "parameters": {"ctest": PHASE_E_BENCHMARK_CTEST, "evidence_grade": "P0"},
        "phase": "benchmark",
        "batch": 0,
        "iteration": 0,
        "status": status,
        "metrics": {
            "return_code": completed.returncode,
            "host_wall_clock_s": host_s,
        },
        "output_hash": None,
        "lowering_report": None,
    }
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
    build = {"schema": "vg.build/v1", "build_dir": build_dir.name, "runner": "benchmark"}
    (run_dir / "environment.json").write_text(canonical_json(environment), encoding="utf-8")
    (run_dir / "build.json").write_text(canonical_json(build), encoding="utf-8")
    (run_dir / "definition.resolved.json").write_text(
        canonical_json(
            {
                "schema": "vg.benchmark/v1",
                "experiment": PHASE_E_BENCHMARK_EXPERIMENT,
                "ctest": PHASE_E_BENCHMARK_CTEST,
                "evidence_grade": "P0",
                "note": "Host wall-clock only. No gpu_ns, hitch, or break-even claim (ADR-042).",
            }
        ),
        encoding="utf-8",
    )
    (run_dir / "stdout.log").write_text(completed.stdout, encoding="utf-8")
    (run_dir / "stderr.log").write_text(completed.stderr, encoding="utf-8")
    (run_dir / "samples.jsonl").write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")
    summary = {
        "schema": "vg.summary/v1",
        "run_id": run_id,
        "status": "ok" if status == "passed" else "failed",
        "experiment": PHASE_E_BENCHMARK_EXPERIMENT,
        "ctest": PHASE_E_BENCHMARK_CTEST,
        "evidence_grade": "P0",
        "host_wall_clock_s": host_s,
        "gpu_ns": None,
        "break_even_curves": "unmeasured",
    }
    (run_dir / "summary.json").write_text(canonical_json(summary), encoding="utf-8")
    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=["experiment", "ctest", "status", "evidence_grade", "host_wall_clock_s"]
        )
        writer.writeheader()
        writer.writerow(
            {
                "experiment": PHASE_E_BENCHMARK_EXPERIMENT,
                "ctest": PHASE_E_BENCHMARK_CTEST,
                "status": status,
                "evidence_grade": "P0",
                "host_wall_clock_s": host_s,
            }
        )
    (run_dir / "report.md").write_text(
        "# Phase E P0 Benchmark Smoke\n\n"
        f"- experiment: {PHASE_E_BENCHMARK_EXPERIMENT}\n"
        f"- ctest: {PHASE_E_BENCHMARK_CTEST}\n"
        f"- status: {status}\n"
        f"- evidence_grade: P0 (demo / single sample; 10 §13)\n"
        f"- host_wall_clock_s: {host_s:.6f}\n"
        "- gpu_ns: not claimed\n"
        "- break-even / hitch: unmeasured\n",
        encoding="utf-8",
    )
    write_manifest(run_dir, run_id)
    return run_dir


def command_benchmark(args: argparse.Namespace) -> int:
    run_dir = create_benchmark_run(Path(args.build_dir).resolve())
    print(run_dir.relative_to(ROOT))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="VG Phase 0 experiment runner")
    subparsers = parser.add_subparsers(dest="command", required=True)
    probe = subparsers.add_parser("probe"); probe.add_argument("--build-dir", required=True); probe.set_defaults(func=command_probe)
    run = subparsers.add_parser("run"); run.add_argument("definition"); run.add_argument("--build-dir", required=True); run.set_defaults(func=command_run)
    analyze = subparsers.add_parser("analyze"); analyze.add_argument("run_dir"); analyze.set_defaults(func=command_analyze)
    for phase in PHASES:
        command = subparsers.add_parser(phase)
        command.add_argument("--build-dir", required=True)
        command.set_defaults(func=command_phase)
    benchmark = subparsers.add_parser("benchmark"); benchmark.add_argument("--build-dir", required=True); benchmark.set_defaults(func=command_benchmark)
    try:
        args = parser.parse_args()
        return args.func(args)
    except (FileNotFoundError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"vg-exp: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
