set(VG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
vg_schema(schemas/ir/task-root.vg.json
  vg_task_root.h vg_task_root.reflection.json vg_task_root.layout.json)
vg_schema(schemas/ir/scene-root-raster.vg.json
  vg_scene_root.h vg_scene_root_layout.h vg_scene_root_msl.h
  vg_scene_root.reflection.json vg_scene_root.layout.json)
vg_schema(schemas/ir/compute-task-ring.vg.json
  vg_compute_task_ring.h vg_compute_task_ring_layout.h vg_compute_task_ring_words.h
  vg_compute_task_ring.reflection.json vg_compute_task_ring.layout.json)
get_property(VG_SCHEMA_OUTPUTS GLOBAL PROPERTY VG_SCHEMA_OUTPUTS)
add_custom_target(vg_schema_generate DEPENDS ${VG_SCHEMA_OUTPUTS})
