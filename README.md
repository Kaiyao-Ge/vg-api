# vg-api

**VG** is a research prototype for a unified GPU semantic layer built around typed GPU
address graphs, Regions, effects, immutable Tasks, certificates, epochs, and
execution envelopes. It is not a driver replacement or an API that bypasses Metal,
Vulkan, or operating-system authority.

This repository contains the public C ABI, portable semantic core, reference and
adapter backends, schema/codegen tools, experiment definitions, and validation
harnesses for the VG project.

## Status

| Phase | Scope |
| --- | --- |
| **Phase 0** | C11 ABI skeleton, CPU/reference enumeration, optional Metal and Linux/Vulkan capability probes, reproducible evidence bundles |
| **Phase A** | Portable semantic core; gate `reference-complete` (E001/E003/E006/E015/E018) |
| **Phase B** | Compute adapters; gate closed per ADR-024 (Metal+reference; Vulkan compile-review-only) |
| **Phase C** | Representation / raster; **not-closed** (layer1 complete; E005/E008/E016 implemented, peak bytes unmeasured) |
| **Phase D** | Dynamic graph / residency research; recorded per ADR-041 (not product-closed) |
| **Phase E** | Research Alpha; recorded per ADR-042 (aggregation + external reproduction, not a product close) |

On Apple Silicon, GPU evidence is **`MetalAdapter`** or **`SemanticReference`** evidence — not evidence of a native VG driver or hardware contract.

External reproduction: [docs/reports/external-repro-runbook.md](docs/reports/external-repro-runbook.md).
Eighteen-experiment table: [docs/reports/phase-e-gate.md](docs/reports/phase-e-gate.md).

## Requirements

- **CMake** ≥ 3.25
- **Ninja** (recommended generator)
- **Python** ≥ 3.10
- **C/C++ compiler** with C11 and C++20 support (Clang recommended)
- **macOS**: Xcode command-line tools for the Metal backend (`dev-metal`, `perf-metal`)
- **Linux**: Vulkan headers and loader for the Vulkan backend (`dev-vulkan`, `perf-vulkan`)

## Quick start

```bash
# Reference backend (CPU, sanitizers on)
cmake --preset dev-reference
cmake --build --preset dev-reference
ctest --preset dev-reference

# macOS Metal adapter
cmake --preset dev-metal
cmake --build --preset dev-metal
ctest --preset dev-metal
python3 tools/vg-exp/vg_exp.py probe --build-dir build/dev-metal
python3 tools/vg-exp/vg_exp.py phase-a --build-dir build/dev-reference
python3 tools/vg-exp/vg_exp.py phase-e --build-dir build/dev-metal
python3 tools/vg-exp/vg_exp.py benchmark --build-dir build/dev-metal

# Schema / link checks
cmake --preset docs
cmake --build --preset docs
ctest --preset docs
```

Linux/NVIDIA hosts use `dev-vulkan`; configuration intentionally fails without
Vulkan headers and loader.

## Repository layout

```text
include/vg/          Public C ABI headers
src/
  api/               C entry points and handle dispatch
  core/              Semantics, lifetime, task/effect/envelope
  ir/                Canonical IR, schema, serialization
  compiler/          Frontend and backend package orchestration
  capture/           Canonical capture/replay
  backends/          reference/, metal/, vulkan/ device HAL adapters
tools/
  vg-schema/         IR schema → C header / reflection codegen
  vg-exp/            Experiment and phase gate runner
  vg-replay/         Capture replay tooling
schemas/             JSON schemas for IR, capture, experiments, runs
experiments/         Experiment definitions and curated workloads
tests/               ABI, unit, model, conformance, and platform tests
docs/                Project charter, architecture, and agent entry point
```

Build outputs go under `build/<preset>/`. Generated headers land in
`build/<preset>/generated/`. Local experiment runs write to `artifacts/runs/` —
both are excluded from version control.

## Documentation

Read **[docs/START.md](docs/START.md)** before changing the project. It is the
single agent and contributor entry point and links to architecture, ABI, backend,
experiment, and roadmap documents under `docs/vg-project/`.

## CMake presets

| Preset | Platform | Purpose |
| --- | --- | --- |
| `dev-reference` | Any | CPU reference backend, Debug + sanitizers |
| `dev-metal` | macOS | Metal adapter, Debug + sanitizers |
| `perf-metal` | macOS | Metal adapter, Release |
| `dev-vulkan` | Linux | Vulkan adapter, Debug + sanitizers |
| `perf-vulkan` | Linux | Vulkan adapter, Release |
| `docs` | Any | Schema and documentation link checks |
| `fuzz` | Any | Fuzzing build with sanitizers |

## Contributing

This is an active research prototype. Follow the specification hierarchy described
in `docs/START.md`: hard boundaries in START > charter/principles > architecture
contracts > backend rules > experiment rules.

Do not commit build directories, generated artifacts, or local run bundles under
`artifacts/runs/`.
