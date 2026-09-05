#ifndef VG_TEST_SUPPORT_VULKAN_ADAPTER_HARNESS_H_
#define VG_TEST_SUPPORT_VULKAN_ADAPTER_HARNESS_H_

#include "backends/vulkan/vulkan_device_hal.h"
#include "backends/vulkan/vulkan_physical_types.h"
#include "compiler/pipeline_classification.h"
#include "vulkan_gpu_experiments.h"

namespace vg::vulkan {

enum class StorageFacetTarget : uint32_t { Image, LinearBuffer };

struct SampleFacetResult {
  std::vector<std::array<float, 4>> sampled_rgba;
  bool facet_cache_hit{};
  // True only when the dispatched pipeline really was specialized with
  // constant_id 0 = true and the generation table/token/violation bindings were
  // written -- i.e. when the in-shader guard of 06 §6.4 actually ran. A
  // FastNative submission reports false rather than letting a caller believe a
  // stale token would have been caught.
  bool checked_generation{};
  uint32_t violation_count{};
  FacetDescriptorCost descriptors;
  vg::hal::LoweringReport report;
};

struct StorageFacetResult {
  std::array<float, 4> written_rgba{};
  bool facet_cache_hit{};
  StorageFacetTarget target{StorageFacetTarget::Image};
  FacetDescriptorCost descriptors;
  vg::hal::LoweringReport report;
};

struct RasterPipelineVariant {
  vg::core::PixelFormat attachment_format{vg::core::PixelFormat::RGBA8Unorm};
  uint32_t sample_count{1};
  std::vector<vg::compiler::StateBlock> state;
};

struct PipelineClassificationResult {
  uint32_t naive_pipeline_count{};
  uint32_t classified_pipeline_count{};
  uint32_t naive_cache_hits{};
  uint32_t classified_cache_hits{};
  uint64_t naive_compile_ns{};
  uint64_t classified_compile_ns{};
  std::vector<vg::compiler::SpecializationReport> classified_specializations;
  vg::hal::LoweringReport report;
};

struct RepresentationPhysicalObservation {
  uint64_t cached_linear_backing_bytes{};
  uint64_t retained_facet_backing_bytes{};
  uint32_t cached_facet_image_count{};
};

// Explicit physical experiments; never an alternative plan assembly path.
class AdapterHarness {
public:
  explicit AdapterHarness(DeviceHal &device) : device_(device) {}
  bool run_sample_facet(const vg::core::Arena &arena, vg::core::FacetPool &pool,
                        vg::core::FacetRef ref, vg::core::FilterMode filter,
                        vg::core::WrapMode wrap,
                        const std::vector<std::array<float, 2>> &uv_coords,
                        float lod, const std::vector<uint32_t> &array_slices,
                        vg::core::ValidationProfile profile,
                        SampleFacetResult *result,
                        std::string *error = nullptr);
  bool run_storage_facet(const vg::core::Arena &arena,
                         vg::core::FacetPool &pool, vg::core::FacetRef ref,
                         StorageFacetTarget target,
                         const std::array<float, 4> &write_rgba,
                         StorageFacetResult *result,
                         std::string *error = nullptr);
  bool run_raster_facet(const vg::core::Arena &arena, vg::core::FacetPool &pool,
                        vg::core::FacetRef attachment_ref,
                        vg::core::FacetRef source_ref,
                        const RasterPassDesc &desc, RasterPassResult *result,
                        std::string *error = nullptr);
  bool run_pipeline_classification(
      const std::vector<RasterPipelineVariant> &variants,
      PipelineClassificationResult *result, std::string *error = nullptr);
  bool observe_representation_backing(const vg::core::Arena &arena,
                                      vg::core::FacetPool &pool,
                                      vg::core::FacetRef retained_ref,
                                      RepresentationPhysicalObservation *result,
                                      std::string *error = nullptr);
  bool
  run_gpu_indirect_experiment(const std::vector<std::array<uint32_t, 3>> &dims,
                              GpuIndirectExperimentResult *result,
                              std::string *error = nullptr) const;
  bool run_gpu_cull_compact_experiment(const std::vector<uint32_t> &visible,
                                       const std::vector<uint32_t> &ids,
                                       GpuCullCompactExperimentResult *result,
                                       std::string *error = nullptr) const;
  bool
  run_gpu_indexed_address_experiment(const std::vector<uint32_t> &input,
                                     GpuIndexedAddressExperimentResult *result,
                                     std::string *error = nullptr) const;
  bool
  run_gpu_tier2_bucket_experiment(const std::vector<uint32_t> &node_classes,
                                  const std::vector<uint32_t> &authorized,
                                  GpuTier2ExperimentResult *result,
                                  std::string *error = nullptr) const;

private:
  DeviceHal &device_;
};

} // namespace vg::vulkan

#endif
