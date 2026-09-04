# Linux / NVIDIA Vulkan build and test runbook

This is a command audit of the existing implementation, not a report of a
successful Linux build or GPU run. The remote machine must supply the evidence.
The current user-reported host has an RTX 4070, RHEL 9, CMake/CTest 3.31.8,
GCC 11.5, Python 3.12.7, Vulkan development packages, and Khronos validation.
`glslc` must finish building before the commands below are used.

## What is actually built

The current [CMake build](../../CMakeLists.txt) uses static libraries, not an
independently installed backend daemon or driver:

```text
Python schema generator -> generated C/layout/reflection files
vg_ir -> vg_core / vg_compiler -> vg_backend_reference / vg_backend_vulkan
vg_core -> vg_capture
vg_api -> core + reference + Vulkan adapter
tools and CTest executables -> the libraries above
Vulkan adapter -> system Vulkan loader -> NVIDIA ICD -> GPU
```

The public API resolves device/Node authority and assembles an immutable
ExecutionPlan. DeviceHAL Stage 6 produces per-Node compute packages and pipeline
objects; Stage 7 dispatches per Task in the sealed effect order. Its GLSL source
is compiled at runtime by `glslc --target-env=vulkan1.2`, not during the C++ build.
`VG_GLSLC_EXECUTABLE` is a CMake cache variable; CMake writes its absolute path into
`VG_GLSLC_PATH` in the compiled backend. Changing PATH later does not replace that
baked-in path. Keep the tool/build directory available, or reconfigure and rebuild.

Use the default ALL build, which includes all enabled test executables, schema
generation, and `vg_e014_capture_fixture`. Building only `vg_backend_vulkan` does
not prepare a complete test run. Fixture generation executes a small CPU helper
during the build.

## Prepare the remote shell

Use a full source checkout in the user's writable scratch directory, including
the Git metadata, tests/fixtures, schemas, tools, docs, and CMake fragments. Do not
copy a macOS build tree. Scratch is not the sole backup of source or results.

Run the following in the repository root on the NVIDIA host. First load the
available Python 3.12 module and put the locally built glslc directory on PATH.
Those two setup commands depend on the site's installation paths. No CUDA module
or CUDA toolkit is required by this CMake build.

```bash
export VG_PYTHON="$(command -v python3.12)"
export VG_GLSLC="$(command -v glslc)"
"$VG_PYTHON" --version
"$VG_GLSLC" --version
```

These variables are only conveniences for the commands below; they are not new
VG runtime settings. Use the system GCC pair so that its already installed
libstdc++/ASan/UBSan libraries match. Do not mix compiler choices in an existing
cache; the fresh configure below replaces CMake's configuration state, not source.

## Configure, build, and run every registered test

This sequence uses the checked Debug preset, keeps logs in a unique ignored run
directory, verifies NVIDIA enumeration using the project's platform executable,
checks that all seven expected Vulkan tests are registered, then runs the entire
CTest suite serially. The platform check is necessary because the existing
`platform.probe --validate` test only requires the reference adapter to exist.
The GPU-independent `compiler.compute-glsl` regression is also required: it
compiles freshly generated compute sources with the configured glslc.

The loader's NVIDIA filename filter prevents llvmpipe from being the first Vulkan
device. If the host uses an unusually named manifest, identify its actual NVIDIA
ICD JSON and use `VK_DRIVER_FILES` with that absolute path instead of the filter.
Do not infer success merely from an empty Vulkan adapter list.

```bash
(
set -euo pipefail
test -f CMakePresets.json
test -x "$VG_PYTHON"
test -x "$VG_GLSLC"

export VK_LOADER_DRIVERS_SELECT='*nvidia*'
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LOADER_DEBUG=error,warn,layer
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

mkdir -p artifacts/runs
VG_RUN="$(mktemp -d "$PWD/artifacts/runs/vulkan-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
printf 'Run directory: %s\n' "$VG_RUN"
git rev-parse HEAD > "$VG_RUN/commit.txt"
git status --short > "$VG_RUN/worktree.txt"
git diff --binary HEAD > "$VG_RUN/changes.patch"
uname -a > "$VG_RUN/kernel.txt"
cat /etc/os-release > "$VG_RUN/os-release.txt"
nvidia-smi > "$VG_RUN/nvidia-smi.txt"
"$VG_GLSLC" --version > "$VG_RUN/glslc-version.txt" 2>&1
printf 'VK_LOADER_DRIVERS_SELECT=%s\nVK_INSTANCE_LAYERS=%s\nVK_LOADER_DEBUG=%s\nUBSAN_OPTIONS=%s\n' \
  "$VK_LOADER_DRIVERS_SELECT" "$VK_INSTANCE_LAYERS" "$VK_LOADER_DEBUG" "$UBSAN_OPTIONS" \
  > "$VG_RUN/test-environment.txt"

cmake --fresh --preset dev-vulkan \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DPython3_EXECUTABLE="$VG_PYTHON" \
  -DVG_GLSLC_EXECUTABLE="$VG_GLSLC" \
  2>&1 | tee "$VG_RUN/configure.log"

cmake --build --preset dev-vulkan --parallel 2 \
  2>&1 | tee "$VG_RUN/build.log"
cp build/dev-vulkan/CMakeCache.txt "$VG_RUN/CMakeCache.txt"
cp build/dev-vulkan/compile_commands.json "$VG_RUN/compile_commands.json"

build/dev-vulkan/vg-platform-probe \
  > "$VG_RUN/platform.json" 2> "$VG_RUN/platform.stderr.log"
"$VG_PYTHON" - "$VG_RUN/platform.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
devices = [a for a in data['adapters'] if a['backend'] == 'vulkan']
print('Vulkan adapters:', devices)
if len(devices) != 1 or 'RTX 4070' not in devices[0]['name']:
    raise SystemExit('Expected exactly the target RTX 4070; inspect ICD selection.')
PY

ctest --preset dev-vulkan --show-only=json-v1 > "$VG_RUN/tests.json"
"$VG_PYTHON" - "$VG_RUN/tests.json" <<'PY'
import json, sys
tests = json.load(open(sys.argv[1]))['tests']
names = {t['name'] for t in tests}
required = {
    'compiler.compute-glsl',
    'conformance.device-hal.vulkan',
    'vertical-slice.vulkan',
    'vertical-slice.vulkan.task-tier0',
    'vertical-slice.vulkan.timeline',
    'vertical-slice.vulkan.raster-rejected',
    'vertical-slice.vulkan.raster-msl-rejected',
    'vertical-slice.vulkan.capability-contract',
}
missing = required - names
if missing:
    raise SystemExit('Missing Vulkan tests: ' + ', '.join(sorted(missing)))
print('Registered tests:', len(tests))
print('Required shader/Vulkan entries:', len(required))
PY

VG_CTEST_RC=0
ctest --preset dev-vulkan --parallel 1 --verbose --no-tests=error \
  --timeout 300 --output-junit "$VG_RUN/ctest.xml" \
  2>&1 | tee "$VG_RUN/ctest.log" || VG_CTEST_RC=$?
printf '%s\n' "$VG_CTEST_RC" > "$VG_RUN/ctest-exit-code.txt"

printf '\nReview validation, sanitizer, and internal-skip messages:\n'
grep -En 'VUID-|Validation Error|AddressSanitizer|LeakSanitizer|runtime error:|skipped \(unsupported\)|support, skipping' \
  "$VG_RUN/platform.stderr.log" "$VG_RUN/ctest.log" || true
printf '\nCTest exit code: %s\nLogs: %s\n' "$VG_CTEST_RC" "$VG_RUN"
exit "$VG_CTEST_RC"
)
```

The exit code above preserves CTest failure through `tee`. It is not an automatic
validation verdict: validation messages and inner skips may coexist with exit 0.
Review the complete logs, including that the loader actually inserted
`VK_LAYER_KHRONOS_validation`; a layer listed by vulkaninfo is not evidence that
the test process enabled it. The grep is a triage aid, not an exhaustive validator.
No sanitizer suppressions or disabled leak detection are applied. A timeout is a
failure to investigate, not a supported skip. Avoid concurrent runs in the same
build directory. Do not reuse these sanitized/validated timings as performance data.

`changes.patch` does not include untracked file contents. If worktree.txt lists
untracked source, retain those files separately with the evidence. These logs
contain local paths and machine details; they are local diagnostics, not a
redacted publication bundle or a new experiment schema.

## Actual Vulkan test coverage

| CTest name | What it establishes |
|---|---|
| `conformance.device-hal.vulkan` | DeviceHAL ABI/capability/compiled-plan checks and reference comparison; some unsupported inner fixtures can print a skip and still return success |
| `vertical-slice.vulkan` | Strict GPU/reference byte comparison for load_only, store_only, atomic_add_only, mixed; unsupported is failure here |
| `vertical-slice.vulkan.task-tier0` | Per-Node packages, per-Task shapes/order, cache reuse, effect barriers and tamper rejection |
| `vertical-slice.vulkan.timeline` | Cross-submission signal/wait plus fault behavior; absent Timeline currently prints an inner skip and returns success |
| `vertical-slice.vulkan.raster-rejected` | Expected rejection of unsupported ExecutionPlan raster work |
| `vertical-slice.vulkan.raster-msl-rejected` | Expected rejection of the unsupported MSL raster route |
| `vertical-slice.vulkan.capability-contract` | Python source-contract regression check; does not execute a GPU workload |

All seven are covered by the full CTest command. A focused diagnostic rerun is:

```bash
ctest --preset dev-vulkan --parallel 1 --verbose --no-tests=error \
  --timeout 300 -R '^(conformance\.device-hal\.vulkan|vertical-slice\.vulkan(\..*)?)$'
```

Run it in a shell with the same driver/layer settings as above (the subshell does
not persist those exports). Keep the original failed log before rerunning.

The full suite also covers C/C++ ABI, portable core/model, compiler/schema,
reference oracle, capture/replay tooling, and documentation. A green reference
raster test is not Vulkan raster evidence. Public tests
`api.c-abi-conformance`, `api.multicode-taskgraph-conformance`,
`api.f6-scene-root-c`, and `api.f7-checkpoint-a-c` select reference on this Linux
configuration. The dedicated Vulkan tests enter internal DeviceHAL directly.
`api.mixed-domain.vulkan` additionally checks the public-ABI Unsupported contract
for raster-containing plans; it is not evidence of Vulkan raster execution.

## Triage of the first RTX 4070 test log (2026-09-04)

The supplied log has 46 tests, 15 failures. They must not be treated as 15
independent backend implementation failures:

- `vertical-slice.vulkan` and `vertical-slice.vulkan.task-tier0` report GLSL
  syntax errors on line 10. The generated `uint64_t` declarations lacked
  `GL_EXT_shader_explicit_arithmetic_types_int64`. The atomic-overload extension
  alone does not enable that type. The linear atomic and indexed generators
  now declare it; load/store-only linear shaders keep their narrower requirements.
- Four linear fixtures and an indexed-store fixture are now fed from the real
  generator into `compiler.compute-glsl`, using the production flags
  `-fshader-stage=compute --target-env=vulkan1.2`. This needs glslc, but no Vulkan
  device or loader. It is registered whenever glslc is found and is mandatory
  in Vulkan builds, which already require that compiler at configure time.
- Many otherwise successful executables fail at exit with LeakSanitizer reports
  containing `libdbus` frames and unloaded/unknown-module frames. Even the
  enumeration-only platform probe fails. VG's normal probe path calls
  `vkDestroyInstance`, and the DeviceHAL destructor destroys its device and
  instance. This does **not** prove that all reported allocations are external:
  the incomplete stacks require a controlled Linux reproduction.
- `tooling.bundle` previously hid the captured probe error behind a Python
  subprocess exit-code traceback. It now prints the error and the saved
  `artifacts/runs/.../stderr.log` path. A failing probe still fails the test;
  valid JSON on stdout must not override a sanitizer failure at process exit.

Keep the original failed logs. Reconfigure and rebuild with the existing
dev-vulkan commands, then isolate the shader check first:

```bash
ctest --preset dev-vulkan --verbose --no-tests=error -R '^compiler\.compute-glsl$'
```

For the leak investigation, the supplied loader trace shows that the optional
`VK_LAYER_MESA_device_select` layer is inserted, and advertises
`NODEVICE_SELECT` as its disable variable. Compare the following two **diagnostic**
probe runs on the same NVIDIA host. The second removes only that optional layer;
it does not disable Khronos validation or any sanitizer. This is an A/B test,
not a confirmed fix and not a change to production defaults.

```bash
(
set -u
export VK_LOADER_DRIVERS_SELECT='*nvidia*'
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LOADER_DEBUG=error,warn,layer
export ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
mkdir -p artifacts/runs
VG_LEAK_RUN="$(mktemp -d "$PWD/artifacts/runs/vulkan-leaks-XXXXXX")"
env -u NODEVICE_SELECT build/dev-vulkan/vg-platform-probe \
  > "$VG_LEAK_RUN/default.json" 2> "$VG_LEAK_RUN/default.stderr.log"
printf '%s\n' "$?" > "$VG_LEAK_RUN/default.exit-code.txt"
NODEVICE_SELECT=1 build/dev-vulkan/vg-platform-probe \
  > "$VG_LEAK_RUN/no-mesa.json" 2> "$VG_LEAK_RUN/no-mesa.stderr.log"
printf '%s\n' "$?" > "$VG_LEAK_RUN/no-mesa.exit-code.txt"
printf 'Diagnostic logs: %s\n' "$VG_LEAK_RUN"
)
```

Check both JSON outputs still enumerate the RTX 4070, both traces insert
`VK_LAYER_KHRONOS_validation`, and only the second trace omits insertion of
`VK_LAYER_MESA_device_select`. Record any other inherited loader settings that
could affect the comparison. If the leak persists without that layer, retain
the deeper stacks and investigate driver/layer ownership; do not add a blanket
libdbus suppression or set `detect_leaks=0`. Even if it disappears, rerun the full
suite under the recorded environment before claiming resolution.

Local verification on Apple M1: the Reference ASan/UBSan suite passed 39/39;
the real-device Metal ASan/UBSan suite passed 73/73 outside the sandbox.
A temporary official glslang 16.5.0 build (commit
`efa016659ffc4f2ae566b6b1db71a70655ac33a1`) compiled all five generated shaders;
removing the new extension reproduced the original line-10 syntax error in both
atomic fixtures and in indexed-store. That is GLSL front-end evidence, **not** a
Linux glslc/NVIDIA execution or LeakSanitizer pass. Remote revalidation is pending.

Local regression commands (from the repository root):

```bash
cmake --preset dev-reference
cmake --build --preset dev-reference --parallel 4
ctest --preset dev-reference --parallel 4 --output-on-failure
cmake --preset dev-metal
cmake --build --preset dev-metal --parallel 4
ctest --preset dev-metal --parallel 1 --output-on-failure
```

The new glslc runner's successful-output, compiler-error, and malformed-SPIR-V
branches were also exercised locally with a mocked compiler and the real source
emitter. That checks the runner contract only; the actual glslc CTest remains a
Linux verification item (expected Vulkan suite size after reconfigure: 47).

The language distinction is defined by Khronos's
[explicit arithmetic types extension](https://github.com/KhronosGroup/GLSL/blob/main/extensions/ext/GL_EXT_shader_explicit_arithmetic_types.txt)
and [64-bit atomic extension](https://github.com/KhronosGroup/GLSL/blob/main/extensions/ext/GL_EXT_shader_atomic_int64.txt).

## Boundaries of a successful run

- The implementation explicitly declines the ExecutionPlan Raster capability;
  standalone facet/raster helper code does not upgrade that contract.
- Mixed compute+raster, pointer-graph and indirect tiers must be judged by their
  implemented capability/entry point, not hardware marketing or old roadmap text.
- [The existing external reproduction runbook](external-repro-runbook.md) targets
  historical Metal/reference Research Alpha evidence. `vg_exp.py phase-b` through
  `phase-e` and `benchmark` retain Metal-specific mappings and/or static Vulkan
  `compile-review-only` classifications. They are not a Vulkan full-test runner.
- `vg_exp.py run` currently packages a platform probe, not all workloads in an
  arbitrary experiment definition. Do not interpret it as full GPU experiments.
- A successful remote run establishes the checked-in tests on the recorded
  machine/commit. It does not itself close all 18 research experiments or update
  historical phase gates.

## Optional optimized build

After the checked test suite passes, build the optimized backend if needed:

```bash
cmake --fresh --preset perf-vulkan \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DPython3_EXECUTABLE="$VG_PYTHON" \
  -DVG_GLSLC_EXECUTABLE="$VG_GLSLC"
cmake --build --preset perf-vulkan --parallel 2
```

There is a configure/build preset named perf-vulkan but no test preset with that
name. Do not use `ctest --preset perf-vulkan`. An explicit
`ctest --test-dir build/perf-vulkan` is possible, but Release defines NDEBUG and
many portable tests use assert, so its success cannot replace the Debug checks.
No existing Vulkan benchmark command supplies a complete performance study.

## External command references

- [Vulkan loader driver selection](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#driver-select-filtering)
- [Vulkan loader layer activation](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md#vk_instance_layers)
- [CTest 3.31 command options](https://cmake.org/cmake/help/v3.31/manual/ctest.1.html)

The current RTX 4070 target differs from the original specifications' 50-series
planning example; the backend uses runtime feature queries rather than that model
label. Consult [START](../START.md) for normative boundaries.
