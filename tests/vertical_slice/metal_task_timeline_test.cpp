#include "metal/cases.h"
#include <iostream>

using namespace vg::tests::metal;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: vg_metal_task_timeline_test "
                 "<task-tier0|timeline|access-certificate|tier1-indirect|cull-compact|cull-compact-1m|effect-dag|pointer-graph|"
                 "indexed-binding|representation-layer|sample-facet|checked-facet-generation|basic-raster|"
                 "task-graph-raster|task-graph-raster-depth|task-graph-raster-user-shader|pipeline-classification|consume-input|"
                 "representation-churn> "
                 "<repo_root>\n";
    return 2;
  }
  const std::string mode = argv[1];
  const std::string root = argv[2];
  bool ok = false;
  if (mode == "task-tier0") {
    ok = run_task_tier0(root);
  } else if (mode == "timeline") {
    ok = run_timeline(root);
  } else if (mode == "access-certificate") {
    ok = run_access_certificate(root);
  } else if (mode == "tier1-indirect") {
    ok = run_tier1_indirect(root);
  } else if (mode == "cull-compact") {
    ok = run_cull_compact(root);
  } else if (mode == "cull-compact-1m") {
    ok = run_cull_compact_1m(root);
  } else if (mode == "effect-dag") {
    ok = run_effect_dag(root);
  } else if (mode == "pointer-graph") {
    ok = run_pointer_graph(root);
  } else if (mode == "indexed-binding") {
    ok = run_indexed_binding(root);
  } else if (mode == "representation-layer") {
    ok = run_representation_layer(root);
  } else if (mode == "sample-facet") {
    ok = run_sample_facet(root);
  } else if (mode == "checked-facet-generation") {
    ok = run_checked_facet_generation(root);
  } else if (mode == "basic-raster") {
    ok = run_basic_raster(root);
  } else if (mode == "task-graph-raster") {
    ok = run_task_graph_raster(root);
  } else if (mode == "task-graph-raster-depth") {
    ok = run_task_graph_raster_depth(root);
  } else if (mode == "task-graph-raster-user-shader") {
    ok = run_task_graph_raster_user_shader(root);
  } else if (mode == "pipeline-classification") {
    ok = run_pipeline_classification(root);
  } else if (mode == "consume-input") {
    ok = run_consume_input(root);
  } else if (mode == "representation-churn") {
    ok = run_representation_churn(root);
  } else {
    std::cerr << "unknown mode: " << mode << "\n";
    return 2;
  }
  return ok ? 0 : 1;
}
