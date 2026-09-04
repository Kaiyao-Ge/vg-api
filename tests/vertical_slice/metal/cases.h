#pragma once
#include <string>
namespace vg::tests::metal {
bool run_task_tier0(const std::string& root);
bool run_timeline(const std::string& root);
bool run_access_certificate(const std::string& root);
bool run_pointer_graph(const std::string& root);
bool run_effect_dag(const std::string& root);
bool run_tier1_indirect(const std::string& root);
bool run_cull_compact(const std::string& root);
bool run_cull_compact_1m(const std::string& root);
bool run_indexed_binding(const std::string& root);
bool run_pipeline_classification(const std::string& root);
bool run_representation_layer(const std::string& root);
bool run_sample_facet(const std::string& root);
bool run_checked_facet_generation(const std::string& root);
bool run_basic_raster(const std::string& root);
bool run_task_graph_raster(const std::string& root);
bool run_task_graph_raster_depth(const std::string& root);
bool run_task_graph_raster_user_shader(const std::string& root);
bool run_consume_input(const std::string& root);
bool run_representation_churn(const std::string& root);
}
