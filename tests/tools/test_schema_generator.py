#!/usr/bin/env python3
from __future__ import annotations
import json, subprocess, sys
from pathlib import Path
root, out = Path(sys.argv[1]), Path(sys.argv[2])
schema = root / "schemas/ir/task-root.vg.json"
subprocess.run([sys.executable, str(root / "tools/vg-schema/vg_schema.py"), str(schema), "--out-dir", str(out)], check=True)
header = (out / "vg_task_root.h").read_text()
assert "VG_SCHEMA_STATIC_ASSERT" in header
assert "_Static_assert(sizeof" not in header
reflection = json.loads((out / "vg_task_root.reflection.json").read_text())
assert reflection["layouts"]["TaskRecord"]["size"] == 48
assert reflection["layouts"]["TaskRecord"]["fields"][0]["offset"] == 0
assert reflection["layouts"]["TaskRecord"]["fields"][-1]["offset"] == 40

scene_schema = root / "schemas/ir/scene-root-raster.vg.json"
subprocess.run([sys.executable, str(root / "tools/vg-schema/vg_schema.py"), str(scene_schema), "--out-dir", str(out)], check=True)
scene_header = (out / "vg_scene_root.h").read_text()
public_header = (root / "include/vg/vg_scene_root.h").read_text()
assert scene_header == public_header, "public SceneRoot header is stale; regenerate it from scene-root-raster.vg.json"
assert "offsetof(VgFacetRef, index) == 0" in scene_header
assert "offsetof(VgFacetRef, generation) == 4" in scene_header
scene_layout = (out / "vg_scene_root_layout.h").read_text()
assert "#include <vg/vg.h>" not in scene_layout
assert "VgSchemaLayout_SceneRootRaster" in scene_layout
assert "VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE" in scene_layout
scene_msl = (out / "vg_scene_root_msl.h").read_text()
assert "VgSchema_SceneRootRaster" in scene_msl
assert "packed_float4 camera_clip_from_local[4]" in scene_msl
metal_source = (root / "src/backends/metal/metal_device_hal.mm").read_text()
assert "newBufferWithLength:VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE" in metal_source
assert "id<MTLBuffer> identity_scene_root_buffer = nil;" in metal_source
assert "std::lock_guard<std::mutex> lock(identity_scene_root_mutex);" in metal_source
assert "if (identity_scene_root_buffer != nil) {" in metal_source
assert "if (created != nullptr) *created = false;" in metal_source
assert "return identity_scene_root_buffer;" in metal_source
assert '"identity_scene_root_buffer_create"' in metal_source
assert '"identity_scene_root_buffer_reuse"' in metal_source
assert "identity_buffer_created ? VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE : 0" in metal_source
assert "reused immutable device-local legacy SceneRoot buffer; no draw allocation" in metal_source
compiler_source = (root / "src/compiler/compute_package.cpp").read_text()
assert "VG_SCHEMA_SCENEROOTRASTER_MSL_DECLARATIONS" in compiler_source
scene_reflection = json.loads((out / "vg_scene_root.reflection.json").read_text())
assert scene_reflection["layouts"]["SceneRootRaster"]["size"] == 64 + scene_reflection["layouts"]["Material"]["size"]
assert scene_reflection["relocations"] == [{"path": ["SceneRootRaster", "material", "albedo"], "kind": "facet", "offset": 80}]
