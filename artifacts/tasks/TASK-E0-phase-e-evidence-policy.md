# TASK-E0: Phase E 证据政策与对外复现合同

Status: complete（文档；runner 在 TASK-E1）。

Normative docs: `docs/START.md` §3–4；`docs/vg-project/12-roadmap-and-risks.md` §6；
`docs/vg-project/09-experiment-catalog.md` Research Alpha 门槛；
`docs/vg-project/10-validation-and-benchmarks.md` §13–15；
ADR-024 / ADR-035 / ADR-041；ADR-042（本任务）。

## Goal

把 Phase E（Research Alpha）的关门标准和对外复现合同先定死。本任务不跑
E001–E018，不加性能数字，只交出：一份 ADR、一份任务总表。

可观察结果：后续任务引用同一套证据规则，不再发明第二套「关门」。

## Invariants

- 不改写 `docs/vg-project/*` 原文。偏离用 ADR Correction，先例是 ADR-024/041。
- 证据政策沿用 B/C/D：Metal + reference 真跑；Vulkan 只 compile-review-only。
- Research Alpha 是汇总 + 复现 + 文档一致，不是新 lowering / 新公共对象。
- 18 行只允许：已有 gate 引用 / `Unsupported` / `Deferred` / `unmeasured`。
- 不把 Phase C `not-closed` 改写成已关门。
- 公共 C ABI 维持 v1.0 最小集。
- 本任务不实现 GPU kernel、不发明测量点、不开 UMD/KMD。

## 建议文件

- `docs/decisions/ADR-042-phase-e-evidence-policy-and-external-reproducibility.md`
- `artifacts/tasks/TASK-E0`–`TASK-E7`

## Tests

`docs.check` 过。无新实验。

## Depends / Unblocks

Depends: Phase D 研究记录（ADR-041）。不要求 Phase C 产品关门。
Unblocks: E1–E7。

## Out of scope

新语义对象；扩展 public C ABI；Vulkan 执行证据；break-even / hitch 曲线；
发行包装；KMD。
