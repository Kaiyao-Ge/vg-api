# TASK-D1: 共用租约、预算、溢出类型

Status: complete（薄 core；submit 尚未消费这些字段）。

Normative docs: `docs/vg-project/02-principles-and-semantics.md` §7、§10、§11；
`docs/vg-project/03-system-architecture.md` Envelope；ADR-035（D0）。

## Goal

在 core 放一套 D2/D3/D5 都会用到的小类型，避免三路各写「工作集 / 租约 / 信封溢出」。
本任务只让类型能被构造、校验、拒绝非法组合；不跑发现扫描，不施压，不续跑。

可观察结果：`core.unit` 能证明：预算 0 与未设预算可区分；租约不能盖住未证明的
allocation；溢出记录不能把「拒收」说成「已续上」。

## Invariants

- Certificate 仍是 sound 上界；Witness 仍是观测，不能替代证明（02 §10）。
- Universe 必须能挂 `universe_budget`；超预算是可预测失败，不是夹断后假装成功。
- 溢出是「这一提交装不下，留给下一提交」，不是静默扩 quota。
- 不在本任务实现 GPU discovery、驱逐、ICB。
- 新增字段默认关闭，不影响现有 `ExecutionPlan` 调用方（先例：E004 的 optional mode）。

## 预计文件

- `src/core/core.h` / `.cpp`：`WorkingSetLease` / 预算 / `EnvelopeOverflow` 一类
  名字以 ADR-035 为准；只含 id、generation、字节上界、是否完整、溢出计数。
- `tests/unit/core_test.cpp`：构造、非法组合、默认关闭。
- 若必须进计划：`src/backends/device_hal.h` 上只加 optional 字段，submit 暂不消费。

## Tests

`core.unit`。不新增 vertical slice。不改 E004/E016 旧断言的含义。

## Depends / Unblocks

Depends: D0 ADR 对名词的约定（可与 D0 同 PR，但 ADR 先合）。
Unblocks: D2（发现要往租约里写子集）、D3（压力打预算）、D5（溢出记录）。

## Risks

- 类型一旦写进 Arena 所有权，后面很难拆。先独立结构体，让 Arena/Plan 引用，
  不要把驱逐策略嵌进 `Allocation`。
- 不要把 Phase B 的 `AccessCertificate` 整形成租约；证书和租约是两件事
  （证明上界 vs 本次驻留）。

## Decision needed

租约是独立类型还是证书上的附加字段：建议独立，ADR-035 写死。

## Out of scope

发现内核；Metal heap/eviction；抓包 schema 变更；公共 C ABI 新函数。
