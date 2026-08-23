# TASK-D6: 抓包可视与 E014（同环境回放）

Status: complete（schema 仍为 `vg.capture/v1` `schema_version` 2；跨后端仅语义对照；动态图因无 discovery API 标 blocked）。

Normative docs: `docs/vg-project/09-experiment-catalog.md` E014；
`docs/vg-project/08-experiment-system.md`；ADR-011（capture v1，跨后端迁移
当时明确是后期）；Phase C 消费后不可恢复（已落地）。

## Goal

canonical 抓包要能定位同一程序，并在兼容环境回放。本任务交付三件事：
能看（可视化或可读报告）、同一设备/参考实现上确定性用例哈希一致、
不兼容能力明确拒绝。

Metal↔Vulkan 互放：只做语义对照（稳定 ID、IR、epoch、fault taxonomy），
不冒充两边都执行过。驱动升级变体有条件才做，没有就标跳过。

可观察结果：计算精确用例抓了能在 reference 回放且哈希一致；光栅用例按
已登记容差比图；消费后的包仍拒绝并报 `cannot restore a consumed representation`；
可视化能列出 allocation / epoch / 故障，不依赖后端地址。

## Invariants

- 可移植抓包不依赖后端地址（09 E014；ADR-011）。
- 改变 capture schema 必须先更新规范/ADR（11 §2）。
- 未知 required 字段拒绝，不修复（ADR-011）。
- 不把 Vulkan 未执行写成跨后端成功。
- 消费后不可写回的语义保持，不回退成泛化 import 失败。

## 预计文件

- `src/capture/capture.h` / `.cpp`：如需进 RepresentationEpoch / raster /
  故障摘要，走 schema 小版本，保持向后可读。
- `tools/vg-replay/` 或新 `tools/vg-capture-view/`：把包打成可读 markdown/json
  报告（可视化的最低交付：稳定 ID、字节、epoch、fault，不必上 GUI）。
- `tests/conformance/phase_a_conformance.cpp` 或新 capture 测试：同环境哈希。
- `tests/vertical_slice/metal_task_timeline_test.cpp`：可选，Metal 抓包后
  在 reference 回放（语义回放，不是 Metal 再跑一遍当跨后端）。
- `experiments/definitions/E014-capture-replay.json`。
- ADR：schema 变更与「跨后端 = 语义对照」的范围。

## Tests

- 计算 exact：serialize → deserialize → replay，哈希一致。
- 故障抓包：poison / PartiallyProduced 仍在。
- 消费后：拒绝 + 固定文案。
- 可视化工具：对一份夹具包输出含稳定 ID，且无 GPU 指针。
- 动态图 / 表示版本：有则测，无则标依赖未就绪，不编数据。

## Depends / Unblocks

Depends: ADR-011 抓包 v1；C 的消费拒绝。动态图可选 D2；多节点可选 D4。
Unblocks: D7 的 E014 行；Phase E 复现包。

## Risks

- 「可视化」被做成重 GUI。最低交付是报告文件，足够 E014 的 stable ID quality。
- 在 Metal 上 replay 抓包容易滑向「用适配器再执行当跨后端」。跨后端成功只允许
  语义字段对齐。

## Decision needed

抓包 schema 是否升到 v2。建议：能加 optional 字段就不升主版本；破坏性变更才 v2。

**Resolved (ADR-040):** 保持 `vg.capture/v1` / `schema_version` 2；view / 能力 /
语义对照字段全部 optional。Metal↔Vulkan 只做语义对照，禁止把两边都写成已执行。

## Out of scope

驱动升级实验室；完整 GUI；把 capture 做成调试器产品；Vulkan 真机回放。

## 交付

- `src/capture/capture.h` / `.cpp`：optional `ViewMetadata`、`ReplayEnvironment`、
  `write_view`；消费后拒绝文案不变。
- `tools/vg-capture-view/`：markdown/json 报告（稳定 ID / epoch / size / fault；
  无 GPU 地址）。
- `tests/unit/capture_view_test.cpp` + ctest `capture.view` / `capture.view.cli`。
- `experiments/definitions/E014-capture-replay.json`（driver-update skipped；
  dynamic-graph blocked）。
- `docs/decisions/ADR-040-capture-view-and-cross-backend-scope.md`。
