# Phase C Gate Report

状态：`not-closed`（Phase C 整体未关闭）  
第一层状态：`layer1-complete`

**撤回说明（2026-08-23）**：此前 `gate-closed-per-adr-030` 的表述已撤回。
完整 Phase C 退出门（E005/E008/E013/E016 + basic raster oracle +
pipeline classification + ConsumeInput）不在本交付范围。

本报告只认定用户要求的**依赖树根 + 第一层**已按原文档落地：

| 工作项 | 状态 | 证据 |
|---|---|---|
| CanonicalView | complete | `core::CanonicalView`；同 view 派生多 kind，不拼最大 ViewRecord |
| Facet pool/generation | complete | `FacetPool` index+generation；epoch mismatch → stale；`retire_stale` |
| Sample / Storage / Attachment | complete | Metal `run_*_facet(FacetRef)` 均经 `FacetPool::lookup`；私有 `facet_map` |
| Representation transform | complete | Private MTLTexture + blit；`Arena::transform`；old/new/temporary 字节与 encoder 计数 |

ctest：`core.unit`、`vertical-slice.metal.representation-layer`。

Phase C 整体退出（`12-roadmap-and-risks.md`）**仍然开放**。
