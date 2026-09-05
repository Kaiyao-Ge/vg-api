# Linux / NVIDIA Vulkan build and test runbook

## Canonical command

The version-controlled test entry point is:

```bash
cd /scratch/kg4178/vg-api
module load python-3.12
export PATH="/scratch/kg4178/tools/shaderc/build/glslc:$PATH"
tools/linux-vulkan-check.sh
```

It supersedes the ad-hoc `test_vulkan.sh`, `test-vulkan.sh`, and
`debug_vulkan_leak.sh` scripts. Keep the checkout clean so that one result maps
to one commit. The script rejects a dirty tree unless `--allow-dirty` is passed
explicitly.

The default gate:

- uses `/usr/bin/gcc` and `/usr/bin/g++` with the `dev-vulkan` preset;
- configures from a fresh CMake cache and builds the default ALL target;
- bakes the resolved Python 3.12 and `glslc` paths into the build;
- selects the NVIDIA ICD and requires a Vulkan adapter name matching `NVIDIA`;
- enables ASan, UBSan, and `VK_LAYER_KHRONOS_validation`;
- audits the 29 Vulkan-related tests frozen by the current parity baseline;
- runs the complete CTest suite serially with a 300-second per-test timeout;
- treats CTest failures, required Vulkan skips, sanitizer findings, validation
  errors, and VUID diagnostics as failures;
- writes full logs under `artifacts/runs/` and prints only a compact terminal
  summary.

No CUDA module or CUDA toolkit is required.

## Current parity scope

At commit `87dcc7c` and its merge commit `c84b18f`, the production Vulkan
Stage 6/7 path contains the same currently implemented semantic families used
by Reference and Metal:

| Family | Vulkan production/test evidence |
| --- | --- |
| Node-aware compute | complete-`NodeRef` packages, per-Task dispatch shape, sealed order, barriers, publication |
| Pointer graph | restricted declared-edge `CachedObject` package plus public C ABI test |
| Access planning | Core-sealed discovery, certificate, working-set budget and lease consumption |
| Representation | facets, checked generation, representation transforms and `ConsumeInput` |
| Raster | built-in direct/indexed triangle list, SceneRoot, RGBA8, optional D32 depth |
| User raster | versioned `vg.glsl.raster/v1`; Metal MSL input rejects on Vulkan |
| Mixed domain | Compute/Raster execution through the Core-sealed schedule |
| Tier2 | sealed complete-`NodeRef` authority, GPU bucket/fill and indirect draw consumption |
| Diagnostics | capability gates, immutable-package checks, `LoweringReport`, result/publication checks |

The checked-in integration reports record Linux 68/68 and Vulkan 29/29 on
llvmpipe with Khronos validation. That is implementation and software-Vulkan
correctness evidence, not RTX 4070 verification or a performance claim. A clean
run of this script on the NVIDIA host is the missing hardware-specific gate.

This parity statement means “caught up with the current Reference/Metal
milestone.” It does not claim implementation of every future item in the
original design documents, automatic sparse faults, arbitrary shader languages,
or new public resource/submit primitives.

## Environment overrides

The script discovers the normal site environment automatically. These variables
are available when the defaults are unsuitable:

```bash
VG_PYTHON=/path/to/python3.12 \
VG_GLSLC=/path/to/glslc \
VG_JOBS=2 \
VG_DEVICE_PATTERN='RTX 4070' \
tools/linux-vulkan-check.sh
```

If the loader's NVIDIA filename filter does not match the installed manifest,
set the official loader override to its absolute path:

```bash
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json \
tools/linux-vulkan-check.sh
```

Do not mix compiler or `glslc` choices in an old build cache. The script uses
`cmake --fresh` and records the resolved versions and CMake cache with every run.

## LeakSanitizer policy

The NVIDIA 580.159.03 user-mode driver has an independently reproduced leak in
the `vkCreateInstance` ICD-loading path. A standalone program with no VG code and
all optional layers disabled reaches:

```text
vkCreateInstance
  -> Vulkan loader ICD scan
  -> libGLX_nvidia / libEGL_nvidia
  -> libnvidia-eglcore.so.580.159.03
  -> dbus_bus_get_private
```

The leak grows by eight allocations per Instance create/destroy cycle and does
not grow when one Instance is reused for repeated enumeration. Therefore the
default correctness gate disables **only** LSan while retaining ASan, UBSan and
Khronos validation. It does not install a broad `libdbus` suppression that could
hide a VG leak.

After a driver/runtime change, run the strict lane:

```bash
tools/linux-vulkan-check.sh --strict-lsan
```

Strict mode enables LSan and uses exit code 23 for a leak. If the platform probe
already fails, the script stops before CTest and records the full report. This is
a failed driver/runtime A/B result, not a waived project pass. Never call
`dbus_shutdown()` or close driver-owned private connections from VG code.

## Evidence and failure triage

Each run records:

```text
artifacts/runs/vulkan-check-<UTC>-<suffix>/
  commit.txt
  worktree.txt
  changes.patch
  kernel.txt
  os-release.txt
  nvidia-smi.txt
  *-version.txt
  test-environment.txt
  configure.log
  build.log
  CMakeCache.txt
  compile_commands.json
  platform.json
  platform.stderr.log
  tests.json
  vulkan-tests.txt
  ctest.xml
  ctest.log
  validation-sanitizer-findings.txt
```

On failure, send the final terminal summary plus that run directory's relevant
logs. The script already extracts CMake/compiler errors, failed test names,
validation messages, VUIDs, and sanitizer reports, so copying the full CTest log
into chat should not be necessary.

The full suite is intentional: Vulkan device tests depend on the same public ABI,
Core assembler, schema, compiler, Reference oracle, capture code and tooling
contracts. Running only `vg_backend_vulkan` or only tests whose names contain
`vulkan` is not a complete integration gate.

## Result boundary

A passing run establishes the checked-in tests on the recorded commit, Linux
environment, loader, NVIDIA driver and selected GPU. It does not establish
performance, every historical experiment row, or unimplemented future design
scope. Sanitized/validated timings must not be reused as benchmark numbers.

External command contracts:

- [Vulkan loader driver selection](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#driver-select-filtering)
- [Vulkan loader layer activation](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md#vk_instance_layers)
- [CTest 3.31 command options](https://cmake.org/cmake/help/v3.31/manual/ctest.1.html)
