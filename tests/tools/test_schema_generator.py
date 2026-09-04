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
metal_resources = (root / "src/backends/metal/metal_resources.mm").read_text()
metal_internal = (root / "src/backends/metal/metal_device_internal.h").read_text()
metal_commit = (root / "src/backends/metal/metal_commit.mm").read_text()
assert "newBufferWithLength:VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE" in metal_resources
assert "id<MTLBuffer> identity_scene_root_buffer = nil;" in metal_internal
assert "std::lock_guard<std::mutex> lock(identity_scene_root_mutex);" in metal_resources
assert "if (identity_scene_root_buffer != nil) {" in metal_resources
assert "if (created != nullptr) *created = false;" in metal_resources
assert "return identity_scene_root_buffer;" in metal_resources
assert '"identity_scene_root_buffer_create"' in metal_commit
assert '"identity_scene_root_buffer_reuse"' in metal_commit
assert "identity_buffer_created ? VG_SCHEMA_SCENEROOTRASTER_ROOT_SIZE : 0" in metal_commit
assert "reused immutable device-local legacy SceneRoot buffer; no draw allocation" in metal_commit
raster_source = (root / "src/compiler/shaders/raster.cpp").read_text()
assert "VG_SCHEMA_SCENEROOTRASTER_MSL_DECLARATIONS" in raster_source
scene_reflection = json.loads((out / "vg_scene_root.reflection.json").read_text())
assert scene_reflection["layouts"]["SceneRootRaster"]["size"] == 64 + scene_reflection["layouts"]["Material"]["size"]
assert scene_reflection["relocations"] == [{"path": ["SceneRootRaster", "material", "albedo"], "kind": "facet", "offset": 80}]

ring_schema = root / "schemas/ir/compute-task-ring.vg.json"
subprocess.run([sys.executable, str(root / "tools/vg-schema/vg_schema.py"), str(ring_schema), "--out-dir", str(out)], check=True)
ring_outputs = {
    name: (out / name).read_text()
    for name in ["vg_compute_task_ring.h", "vg_compute_task_ring_layout.h",
                 "vg_compute_task_ring_words.h", "vg_compute_task_ring.reflection.json",
                 "vg_compute_task_ring.layout.json"]
}
# Re-emitting into the same directory must be byte deterministic.
subprocess.run([sys.executable, str(root / "tools/vg-schema/vg_schema.py"), str(ring_schema), "--out-dir", str(out)], check=True)
for name, contents in ring_outputs.items():
    assert (out / name).read_text() == contents

ring_reflection = json.loads(ring_outputs["vg_compute_task_ring.reflection.json"])
ring_fields = ring_reflection["layouts"]["ComputeTaskRingRecord"]["fields"]
assert ring_reflection["layouts"]["ComputeTaskRingRecord"]["size"] == 14 * 4
assert [field["offset"] for field in ring_fields] == list(range(0, 14 * 4, 4))
ring_layout = ring_outputs["vg_compute_task_ring_layout.h"]
assert "VG_SCHEMA_COMPUTETASKRING_COMPUTETASKRINGRECORD_X_OFFSET 20u" in ring_layout
ring_words = ring_outputs["vg_compute_task_ring_words.h"]
assert "inline constexpr uint32_t kWordCount = 14u;" in ring_words
assert 'inline constexpr std::string_view kByteOrder = "little-endian";' in ring_words
assert "std::endian::native == std::endian::little" in ring_words
assert "inline constexpr uint32_t kXWord = 5u;" in ring_words
assert "inline constexpr uint32_t kYWord = 6u;" in ring_words
assert "inline constexpr uint32_t kZWord = 7u;" in ring_words
assert "inline constexpr uint32_t kReservedWord = 11u;" in ring_words
assert '{"reserved", 11u, true}' in ring_words
assert "#define VG_TASK_RING_WORD_COUNT 14u" in ring_words
assert "#define VG_TASK_RING_X_WORD 5u" in ring_words

# Both shader dialects splice the same generated fragment, and active backend
# code owns neither a local codec nor numeric x/node offsets.
ring_source = (root / "src/compiler/shaders/task_ring.cpp").read_text()
assert ring_source.count("schema::compute_task_ring::kShaderLayout") == 2
assert "word < 14u" not in ring_source
metal_sources = [path.read_text() for path in (root / "src/backends/metal").glob("*.mm")]
metal_sources.append((root / "tests/support/metal_adapter_harness.mm").read_text())
metal_encoding = (root / "src/backends/metal/metal_encoding.mm").read_text()
vulkan_sources = [path.read_text() for path in (root / "src/backends/vulkan").glob("*.cpp")]
vulkan_sources.append((root / "tests/support/vulkan_adapter_harness.cpp").read_text())
vulkan_resources = (root / "src/backends/vulkan/vulkan_resources.cpp").read_text()
assert all("void pack_task_record" not in source for source in metal_sources)
assert all("unpack_task_record(" not in source for source in metal_sources)
assert all("void pack_task_record" not in source for source in vulkan_sources)
assert all("unpack_task_record(" not in source for source in vulkan_sources)
assert "kTaskRingDispatchXWord" in metal_encoding
assert "kTaskRingDispatchXWord" in vulkan_resources
assert "kTaskRingNodeIndexWord" in (root / "src/backends/metal/metal_tier2.mm").read_text()
