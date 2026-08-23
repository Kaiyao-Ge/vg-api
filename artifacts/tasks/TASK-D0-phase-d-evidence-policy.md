# TASK-D0: Phase D 证据政策与共用合同

Status: complete（文档；类型在 TASK-D1）。

Normative docs: `docs/START.md` §3–4；`docs/vg-project/12-roadmap-and-risks.md` §5；
`docs/vg-project/09-experiment-catalog.md` 门槛表 Representation 之后的
Research 工作（E004/E010/E011/E014/E017）；ADR-024 / ADR-030（证据政策先例）；
ADR-025 Revisit（DiscoverThenLease 不得再退化成 Universe）。

## Goal

把 Phase D 的关门标准和四路并行共用的名词先定死，避免每条实验各写一套
「租约 / 预算 / 溢出」。本任务不跑 E004–E017，只交出：一份 ADR、一份任务总表、
以及（若必须）core 里默认关闭的空类型，让 D1–D6 能同时开工。

可观察结果：后续任务引用同一套证据规则和同一组核心类型，不再发明第二套。

## Invariants

- 不改写 `docs/vg-project/*` 原文。需要偏离时用 ADR Correction 旁注，先例是 ADR-024。
- 证据政策沿用 B/C：Metal + reference 真跑；Vulkan 只 compile-review-only，
  不冒充执行证据。
- `HostAssisted` / `Unsupported` 是 D 的合法结论，不是失败。
- 不把统一内存写成无限内存；不把 Vulkan sparse 写成自动 fault。
- 公共 ABI 不新增纹理对象；令牌语义保持 Phase C 约定。
- 本任务不实现 GPU discovery、ICB、稀疏页、抓包可视化。

## 建议文件

- `docs/decisions/ADR-035-phase-d-evidence-policy-and-shared-contracts.md`
  （新）：D 关门标准、五门实验与四路分工、HostAssisted 边界清单与
  NativeContractResearch v1 的含义、明确「B 的 E004 不是 D 的 E004」。
- `artifacts/tasks/TASK-D0`–`TASK-D7`：本目录这批任务单。
- 仅当 D1/D2/D5 会立刻抢同一头文件时，才在 `src/core/core.h` 增加默认关闭的
  空类型（租约 / 预算 / 溢出记录）。能拖到 D1 的不要在这里发明实现。

## Tests

无新实验。若动了 core 空类型：现有 `core.unit` 必须仍过。`docs.check` 过。

## Depends / Unblocks

Depends: Phase C 主体已在 Metal+reference 上可跑（当前分支）。
Unblocks: D1–D6 并行。

## Risks

- 把 B 的 E004 定义文件直接改成 D 结果，会抹掉当时的诚实退化结论。D 应新增
  定义或在 judgement 里写「重访」，不假装 B 没做过。
- 共用类型若一次做满，会拖住四路。宁可类型薄、实验里填语义。

## Decision needed

无。证据政策复用 ADR-024/030；E004 重访范围以 ADR-025 Revisit 为准。

## Out of scope

E004 盈亏曲线；E010 ICB；E011 压力测量；E014 可视化；E017 续跑；
Phase E 对外复现。
