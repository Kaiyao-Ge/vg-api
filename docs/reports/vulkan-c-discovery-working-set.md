# Vulkan C: Core-sealed discovery and working-set consumption

Date: 2026-09-04. Status: complete and integrated in the production Vulkan Stage 6/7 path.

## Contract and implementation disposition

`ExecutionPlanAssembler` completes the discovery walk, freezes the
`GraphEpoch`, certificate, witness, and optional lease, and validates the
working-set request before Stage 6. The Vulkan Stage 7 path already calls
`run_discovery_stage` and `apply_working_set_budget`. Both helpers consume
the sealed `ExecutionPlan` fields and only emit reports: neither reads Arena
bytes to rebuild topology, reachability, certificate, touched set, schedule,
or lease authority.

The baseline Vulkan lowering rejected every discovery seed and every
ordinary working-set budget or lease. The integrated minimal change removes
those broad guards. It keeps `SoftwarePaged` and
`FaultManaged` explicitly `Unsupported`; accepting ordinary allocation-size
budget proxies does not advertise sparse residency, automatic faults, or OS
residency truth. The shared helper records `working_set_sparse` as
`Unsupported` and labels requested/committed/proxy bytes as proxies.

## Test contract

`tests/vertical_slice/vulkan_discovery_working_set_test.cpp` has two CLI
GPU modes, centrally registered as exactly these CTests:

| CTest | command mode | assertion |
| --- | --- | --- |
| `vertical-slice.vulkan.discovery` | `discovery <repo_root>` | real Core assembly followed by Vulkan compile/submit and Reference oracle; a 2-node seed-reachable subset is smaller than four active allocations; Vulkan reports the frozen certificate as `HostAssisted`. |
| `vertical-slice.vulkan.working-set` | `working-set <repo_root>` | real Vulkan and Reference compile/submit accept exact-budget and lease-only plans; reports carry allocation-size proxy bytes and sparse `Unsupported`; over-budget and stale lease inputs fail in core assembly before device work. |

The discovery operation uses a canonical linear store to reachable allocation
`n0`, so the test does not depend on pointer-graph lowering while still
requiring observable GPU work. It never fabricates a sealed plan, falls back
to Reference on missing Vulkan hardware, or turns absence of a device into a
skip. The Reference result is a parallel oracle, never Vulkan fallback.
`reference-fixture-only <repo_root>` is an unregistered CPU-only diagnostic
mode for local semantic validation; its output explicitly says that no Vulkan
work ran. It executes both the discovery linear-store fixture and the
restricted static-edge pointer-plus-discovery fixture through Reference,
including their byte checks.

## Integrated evidence

`vertical-slice.vulkan.discovery` and
`vertical-slice.vulkan.working-set` execute through real Vulkan compile/submit
on llvmpipe. They consume Core-sealed discovery, certificate, budget, and lease
facts without rebuilding authority in the backend. The final checkout passes
68/68 Linux tests and all 29 Vulkan tests under Khronos validation with zero
validation errors or VUID diagnostics. Reference ASan/UBSan passes 45/45 and
Metal ASan/UBSan passes 79/79.
