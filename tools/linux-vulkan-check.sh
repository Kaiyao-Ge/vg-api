#!/usr/bin/env bash

# Canonical Linux Vulkan build and correctness gate.
#
# Default: ASan + UBSan + Khronos validation with LSan disabled because the
# NVIDIA 580.159.03 ICD has an independently reproduced per-VkInstance D-Bus
# leak.  --strict-lsan deliberately removes that exception for driver A/B
# testing and must fail when the leak is present.

set -uo pipefail

usage() {
  cat <<'EOF'
Usage: tools/linux-vulkan-check.sh [--strict-lsan] [--allow-dirty]

Environment overrides:
  VG_PYTHON          Python 3.12 executable
  VG_GLSLC           glslc executable
  VG_JOBS            build parallelism (default: 2)
  VG_DEVICE_PATTERN  required Vulkan adapter-name regex (default: NVIDIA)
  VK_DRIVER_FILES    optional absolute ICD manifest path

Default mode is the project correctness gate. It retains ASan, UBSan and the
Khronos validation layer, but disables LeakSanitizer only. Use --strict-lsan
to test whether a driver/runtime update has removed the independently
reproduced NVIDIA ICD leak; strict mode never converts a leak into a pass.
EOF
}

VG_STRICT_LSAN=0
VG_ALLOW_DIRTY=0
while (($#)); do
  case "$1" in
    --strict-lsan) VG_STRICT_LSAN=1 ;;
    --allow-dirty) VG_ALLOW_DIRTY=1 ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

VG_SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VG_REPO_ROOT="$(cd -- "$VG_SCRIPT_DIR/.." && pwd)"
cd "$VG_REPO_ROOT" || exit 2

if [[ "$(uname -s)" != Linux ]]; then
  printf 'This gate requires Linux.\n' >&2
  exit 2
fi
for VG_TOOL in git cmake ctest ninja /usr/bin/gcc /usr/bin/g++; do
  if ! command -v "$VG_TOOL" >/dev/null 2>&1; then
    printf 'Missing required tool: %s\n' "$VG_TOOL" >&2
    exit 2
  fi
done

# Site modules are optional. Load the known Python module only when Python 3.12
# is not already available; callers may avoid this entirely with VG_PYTHON.
if [[ -z "${VG_PYTHON:-}" ]] && ! command -v python3.12 >/dev/null 2>&1; then
  if command -v module >/dev/null 2>&1; then
    module load python-3.12 || true
  fi
fi
VG_PYTHON_BIN="${VG_PYTHON:-$(command -v python3.12 || true)}"
VG_GLSLC_BIN="${VG_GLSLC:-$(command -v glslc || true)}"
VG_BUILD_JOBS="${VG_JOBS:-2}"
VG_REQUIRED_DEVICE="${VG_DEVICE_PATTERN:-NVIDIA}"

if [[ -z "$VG_PYTHON_BIN" || ! -x "$VG_PYTHON_BIN" ]]; then
  printf 'Python 3.12 was not found; load the site module or set VG_PYTHON.\n' >&2
  exit 2
fi
if [[ -z "$VG_GLSLC_BIN" || ! -x "$VG_GLSLC_BIN" ]]; then
  printf 'glslc was not found; add it to PATH or set VG_GLSLC.\n' >&2
  exit 2
fi
if [[ ! "$VG_BUILD_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  printf 'VG_JOBS must be a positive integer.\n' >&2
  exit 2
fi

VG_COMMIT="$(git rev-parse HEAD)"
VG_DIRTY="$(git status --porcelain=v1)"
if [[ -n "$VG_DIRTY" && "$VG_ALLOW_DIRTY" -eq 0 ]]; then
  printf 'Working tree is not clean; commit/stash changes or use --allow-dirty.\n%s\n' "$VG_DIRTY" >&2
  exit 2
fi

mkdir -p artifacts/runs
VG_RUN_DIR="$(mktemp -d "$VG_REPO_ROOT/artifacts/runs/vulkan-check-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
printf '%s\n' "$VG_COMMIT" > "$VG_RUN_DIR/commit.txt"
git status --porcelain=v1 > "$VG_RUN_DIR/worktree.txt"
git diff --binary HEAD > "$VG_RUN_DIR/changes.patch"
uname -a > "$VG_RUN_DIR/kernel.txt"
[[ -r /etc/os-release ]] && cp /etc/os-release "$VG_RUN_DIR/os-release.txt"
command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi > "$VG_RUN_DIR/nvidia-smi.txt" 2>&1
"$VG_PYTHON_BIN" --version > "$VG_RUN_DIR/python-version.txt" 2>&1
"$VG_GLSLC_BIN" --version > "$VG_RUN_DIR/glslc-version.txt" 2>&1
/usr/bin/gcc --version > "$VG_RUN_DIR/gcc-version.txt" 2>&1
cmake --version > "$VG_RUN_DIR/cmake-version.txt" 2>&1

export VK_LOADER_DEBUG=error,warn,layer
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
unset VK_LOADER_LAYERS_DISABLE VK_LOADER_LAYERS_ENABLE VK_LOADER_LAYERS_ALLOW
if [[ -z "${VK_DRIVER_FILES:-}" ]]; then
  export VK_LOADER_DRIVERS_SELECT='*nvidia*'
else
  unset VK_LOADER_DRIVERS_SELECT
fi
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
if [[ "$VG_STRICT_LSAN" -eq 1 ]]; then
  export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:fast_unwind_on_malloc=0:malloc_context_size=50
  export LSAN_OPTIONS=exitcode=23:report_objects=1
  VG_MODE=strict-lsan
else
  export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
  unset LSAN_OPTIONS
  VG_MODE=functional
fi
env | grep -E '^(ASAN_OPTIONS|LSAN_OPTIONS|UBSAN_OPTIONS|VK_DRIVER_FILES|VK_INSTANCE_LAYERS|VK_LOADER_|VG_DEVICE_PATTERN)=' \
  | sort > "$VG_RUN_DIR/test-environment.txt"

printf 'Commit: %s\nMode: %s\nLogs: %s\n' "$VG_COMMIT" "$VG_MODE" "$VG_RUN_DIR"

VG_CONFIGURE_RC=0
printf '[1/5] Configure ...\n'
cmake --fresh --preset dev-vulkan \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DPython3_EXECUTABLE="$VG_PYTHON_BIN" \
  -DVG_GLSLC_EXECUTABLE="$VG_GLSLC_BIN" \
  > "$VG_RUN_DIR/configure.log" 2>&1 || VG_CONFIGURE_RC=$?
if [[ "$VG_CONFIGURE_RC" -ne 0 ]]; then
  printf 'Configure failed (exit %s). Key output:\n' "$VG_CONFIGURE_RC" >&2
  grep -En 'CMake Error|Could NOT find|not found|error:' "$VG_RUN_DIR/configure.log" | tail -n 80 >&2 || true
  printf 'Full log: %s/configure.log\n' "$VG_RUN_DIR" >&2
  exit "$VG_CONFIGURE_RC"
fi

VG_BUILD_RC=0
printf '[2/5] Build all targets ...\n'
cmake --build --preset dev-vulkan --parallel "$VG_BUILD_JOBS" \
  > "$VG_RUN_DIR/build.log" 2>&1 || VG_BUILD_RC=$?
if [[ "$VG_BUILD_RC" -ne 0 ]]; then
  printf 'Build failed (exit %s). Key output:\n' "$VG_BUILD_RC" >&2
  grep -En 'FAILED:|fatal error:|error:|undefined reference|ninja: build stopped' "$VG_RUN_DIR/build.log" | tail -n 120 >&2 || true
  printf 'Full log: %s/build.log\n' "$VG_RUN_DIR" >&2
  exit "$VG_BUILD_RC"
fi
cp build/dev-vulkan/CMakeCache.txt "$VG_RUN_DIR/CMakeCache.txt"
cp build/dev-vulkan/compile_commands.json "$VG_RUN_DIR/compile_commands.json"

VG_PROBE_RC=0
printf '[3/5] Verify selected Vulkan adapter ...\n'
build/dev-vulkan/vg-platform-probe > "$VG_RUN_DIR/platform.json" \
  2> "$VG_RUN_DIR/platform.stderr.log" || VG_PROBE_RC=$?
if [[ "$VG_PROBE_RC" -ne 0 ]]; then
  printf 'Platform probe failed (exit %s).\n' "$VG_PROBE_RC" >&2
  grep -En 'VUID-|Validation Error|AddressSanitizer|LeakSanitizer|runtime error:|ERROR:' \
    "$VG_RUN_DIR/platform.stderr.log" | tail -n 100 >&2 || true
  if [[ "$VG_STRICT_LSAN" -eq 1 && "$VG_PROBE_RC" -eq 23 ]]; then
    printf 'Strict LSan detected a Vulkan loader/ICD leak before project tests. This is a failure, not a waiver.\n' >&2
  fi
  printf 'Full logs: %s/platform.*\n' "$VG_RUN_DIR" >&2
  exit "$VG_PROBE_RC"
fi
if ! grep -Eq 'Insert instance layer.*VK_LAYER_KHRONOS_validation' \
    "$VG_RUN_DIR/platform.stderr.log"; then
  printf 'The loader log does not prove that VK_LAYER_KHRONOS_validation was inserted.\n' >&2
  printf 'Inspect %s/platform.stderr.log\n' "$VG_RUN_DIR" >&2
  exit 1
fi
"$VG_PYTHON_BIN" - "$VG_RUN_DIR/platform.json" "$VG_REQUIRED_DEVICE" <<'PY'
import json
import re
import sys

path, required = sys.argv[1:]
with open(path, encoding="utf-8") as stream:
    data = json.load(stream)
devices = [adapter for adapter in data.get("adapters", [])
           if adapter.get("backend") == "vulkan"]
if not devices:
    raise SystemExit("No Vulkan adapter was enumerated")
matched = [device for device in devices
           if re.search(required, str(device.get("name", "")), re.IGNORECASE)]
print("Vulkan adapters:", ", ".join(str(d.get("name", "")) for d in devices))
if not matched:
    raise SystemExit(f"No Vulkan adapter name matches /{required}/")
PY
VG_ADAPTER_RC=$?
if [[ "$VG_ADAPTER_RC" -ne 0 ]]; then
  printf 'Adapter verification failed; inspect %s/platform.json\n' "$VG_RUN_DIR" >&2
  exit "$VG_ADAPTER_RC"
fi

printf '[4/5] Audit registered test inventory ...\n'
ctest --preset dev-vulkan --show-only=json-v1 > "$VG_RUN_DIR/tests.json"
"$VG_PYTHON_BIN" - "$VG_RUN_DIR/tests.json" "$VG_RUN_DIR/vulkan-tests.txt" <<'PY'
import json
import sys

inventory_path, output_path = sys.argv[1:]
with open(inventory_path, encoding="utf-8") as stream:
    registered = {test["name"] for test in json.load(stream)["tests"]}
if len(registered) != 68:
    raise SystemExit(
        f"Expected the 68-test parity baseline, found {len(registered)}; "
        "inspect tests.json and update this gate only for an intentional inventory change"
    )
expected = {
    "api.mixed-domain.vulkan",
    "api.vulkan-pointer-graph",
    "conformance.device-hal.vulkan",
    "unit.vulkan-draw-experiments",
    "unit.vulkan-tier2-handoff",
    "unit.vulkan-user-raster-contract",
    "vertical-slice.vulkan",
    "vertical-slice.vulkan.capability-contract",
    "vertical-slice.vulkan.consume-input",
    "vertical-slice.vulkan.cull-compact",
    "vertical-slice.vulkan.discovery",
    "vertical-slice.vulkan.facet-raster",
    "vertical-slice.vulkan.facets",
    "vertical-slice.vulkan.indexed-address",
    "vertical-slice.vulkan.indirect",
    "vertical-slice.vulkan.pipeline-classification",
    "vertical-slice.vulkan.plan-depth-scene",
    "vertical-slice.vulkan.plan-tier2",
    "vertical-slice.vulkan.pointer-graph",
    "vertical-slice.vulkan.raster-basic",
    "vertical-slice.vulkan.raster-msl-rejected",
    "vertical-slice.vulkan.representation",
    "vertical-slice.vulkan.task-tier0",
    "vertical-slice.vulkan.tier2",
    "vertical-slice.vulkan.timeline",
    "vertical-slice.vulkan.user-glsl-device",
    "vertical-slice.vulkan.working-set",
    "vertical-slice.vulkan-plan-raster-cpu",
    "vertical-slice.vulkan-plan-raster-source",
}
missing = sorted(expected - registered)
if missing:
    raise SystemExit("Missing required Vulkan tests: " + ", ".join(missing))
for common_gate in ("compiler.compute-glsl", "core.execution-plan", "docs.check"):
    if common_gate not in registered:
        raise SystemExit("Missing shared integration gate: " + common_gate)
with open(output_path, "w", encoding="utf-8") as stream:
    stream.write("\n".join(sorted(expected)) + "\n")
print(f"Registered tests: {len(registered)}; required Vulkan tests: {len(expected)}")
PY
VG_INVENTORY_RC=$?
if [[ "$VG_INVENTORY_RC" -ne 0 ]]; then
  exit "$VG_INVENTORY_RC"
fi

VG_CTEST_RC=0
printf '[5/5] Run the complete test suite ...\n'
ctest --preset dev-vulkan --parallel 1 --no-tests=error --timeout 300 \
  --output-on-failure --output-junit "$VG_RUN_DIR/ctest.xml" \
  > "$VG_RUN_DIR/ctest.log" 2>&1 || VG_CTEST_RC=$?
printf '%s\n' "$VG_CTEST_RC" > "$VG_RUN_DIR/ctest-exit-code.txt"

VG_DIAGNOSTIC_RC=0
if grep -En 'VUID-[A-Za-z0-9]|Validation Error:|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:' \
    "$VG_RUN_DIR/platform.stderr.log" "$VG_RUN_DIR/ctest.log" \
    > "$VG_RUN_DIR/validation-sanitizer-findings.txt"; then
  VG_DIAGNOSTIC_RC=1
fi

"$VG_PYTHON_BIN" - "$VG_RUN_DIR/ctest.xml" "$VG_RUN_DIR/vulkan-tests.txt" <<'PY'
import sys
import xml.etree.ElementTree as ET

xml_path, required_path = sys.argv[1:]
required = {line.strip() for line in open(required_path, encoding="utf-8") if line.strip()}
root = ET.parse(xml_path).getroot()
cases = list(root.iter("testcase"))
failed = []
skipped = []
for case in cases:
    name = case.get("name", "<unnamed>")
    if case.find("failure") is not None or case.find("error") is not None:
        failed.append(name)
    if case.find("skipped") is not None:
        skipped.append(name)
required_skips = sorted(required.intersection(skipped))
print(f"CTest summary: total={len(cases)} failed={len(failed)} skipped={len(skipped)}")
if failed:
    print("Failed tests:", ", ".join(failed))
if required_skips:
    print("ERROR: required Vulkan tests skipped:", ", ".join(required_skips))
    raise SystemExit(1)
PY
VG_JUNIT_RC=$?

if [[ "$VG_CTEST_RC" -ne 0 ]]; then
  printf 'CTest failed (exit %s). Failure summary:\n' "$VG_CTEST_RC" >&2
  grep -En 'The following tests FAILED:|[0-9]+ - .*\(Failed\)|FAILED|Subprocess aborted|Segmentation fault' \
    "$VG_RUN_DIR/ctest.log" | tail -n 120 >&2 || true
fi
if [[ "$VG_DIAGNOSTIC_RC" -ne 0 ]]; then
  printf 'Validation/sanitizer findings:\n' >&2
  tail -n 120 "$VG_RUN_DIR/validation-sanitizer-findings.txt" >&2
fi

printf '\nResult: configure=%s build=%s probe=%s ctest=%s diagnostics=%s required-skips=%s\n' \
  "$VG_CONFIGURE_RC" "$VG_BUILD_RC" "$VG_PROBE_RC" "$VG_CTEST_RC" \
  "$VG_DIAGNOSTIC_RC" "$VG_JUNIT_RC"
printf 'Complete evidence: %s\n' "$VG_RUN_DIR"

if [[ "$VG_CTEST_RC" -ne 0 || "$VG_DIAGNOSTIC_RC" -ne 0 || "$VG_JUNIT_RC" -ne 0 ]]; then
  exit 1
fi
printf 'PASS: complete Linux Vulkan correctness gate passed on the selected adapter.\n'
