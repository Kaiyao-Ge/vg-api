#ifndef VG_BACKENDS_VULKAN_DEVICE_HAL_H_
#define VG_BACKENDS_VULKAN_DEVICE_HAL_H_

#include "backends/device_hal.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(VG_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace vg::vulkan {

// Owns the Vulkan instance/device objects used by the adapter.  Backend handles
// remain private; callers only observe the versioned DeviceHal contract.
class DeviceHal final : public vg::hal::DeviceHal {
 public:
  ~DeviceHal() override;

  DeviceHal(const DeviceHal&) = delete;
  DeviceHal& operator=(const DeviceHal&) = delete;

  const vg::hal::CapabilitySnapshot& capabilities() const override;
  bool compile(const vg::hal::ExecutionPlan& plan,
               vg::hal::CompiledPlan* compiled,
               std::string* error = nullptr) override;
  bool submit(const vg::hal::CompiledPlan& compiled, vg::core::Arena& arena,
              vg::hal::Submission* submission,
              std::string* error = nullptr) override;

#if defined(VG_HAS_VULKAN)
  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical_device() const { return physical_device_; }
  VkDevice device() const { return device_; }
  uint32_t compute_queue_family() const { return compute_queue_family_; }
#endif

 private:
  DeviceHal() = default;

  vg::hal::CapabilitySnapshot capabilities_;
#if defined(VG_HAS_VULKAN)
  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue compute_queue_{VK_NULL_HANDLE};
  uint32_t compute_queue_family_{UINT32_MAX};

  // GPU-side buffer VG mints for a given core::Allocation, addressed via
  // buffer device address (BDA) rather than a descriptor set. `mapped` stays
  // valid for the buffer's lifetime: memory is host-visible-coherent, so no
  // explicit flush/invalidate is needed around dispatch (v1 simplification --
  // no staging buffer, mirrors Metal's Shared-storage-mode choice).
  struct AllocationRecord {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceAddress device_address{};
    void* mapped{nullptr};
    uint32_t generation{};
    size_t byte_size{};
  };

  VkShaderModule shader_module_{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline compute_pipeline_{VK_NULL_HANDLE};
  VkCommandPool command_pool_{VK_NULL_HANDLE};
  std::string cached_ir_hash_;
  std::unordered_map<uint64_t, AllocationRecord> allocation_map_;

  // VK_SEMAPHORE_TYPE_TIMELINE, initialValue=0; created lazily on first
  // timeline_wait/timeline_signal use (mirrors Metal's lazily-created
  // MTLSharedEvent). Its counter, queried via vkGetSemaphoreCounterValue, is
  // the single source of truth for this device's timeline position -- no
  // separate host-side mirror is kept, so nothing can drift out of sync
  // with what the GPU actually reached.
  VkSemaphore timeline_semaphore_{VK_NULL_HANDLE};

  // Task ring publication kernel (Tier0), compiled from
  // compiler::task_ring_vulkan_source() into its own shader module/layout/
  // pipeline, kept separate from shader_module_/pipeline_layout_/
  // compute_pipeline_ above: the publication protocol is backend-private
  // infrastructure, not part of the target-neutral ComputePackage contract
  // (mirrors Metal's task_ring_library/task_ring_pipeline separation).
  VkShaderModule task_ring_shader_module_{VK_NULL_HANDLE};
  VkPipelineLayout task_ring_pipeline_layout_{VK_NULL_HANDLE};
  VkPipeline task_ring_pipeline_{VK_NULL_HANDLE};

  // Ephemeral GPU-side buffers for one task-graph submission. Sized to the
  // task count and recreated per submit() call rather than cached in
  // allocation_map_, since task_graph size varies call to call and these
  // buffers are backend-private (never exposed through core::Allocation).
  struct TaskRingBuffers {
    VkBuffer state_buffer{VK_NULL_HANDLE};
    VkDeviceMemory state_memory{VK_NULL_HANDLE};
    VkDeviceAddress state_address{};
    void* state_mapped{nullptr};
    VkBuffer fields_buffer{VK_NULL_HANDLE};
    VkDeviceMemory fields_memory{VK_NULL_HANDLE};
    VkDeviceAddress fields_address{};
    void* fields_mapped{nullptr};
    VkBuffer inputs_buffer{VK_NULL_HANDLE};
    VkDeviceMemory inputs_memory{VK_NULL_HANDLE};
    VkDeviceAddress inputs_address{};
    void* inputs_mapped{nullptr};
    // Tier1 conformance floor: one VkDispatchIndirectCommand-sized (12-byte,
    // 4-byte-aligned) slot per task, populated from fields_buffer's x/y/z
    // words via vkCmdCopyBuffer -- see dispatch_task_ring_and_tier1's doc
    // comment for why no host-side repacking is needed.
    VkBuffer indirect_buffer{VK_NULL_HANDLE};
    VkDeviceMemory indirect_memory{VK_NULL_HANDLE};
    uint32_t task_count{};
  };

  // (Re)compiles `glsl_source` (GLSL -> SPIR-V via a glslc subprocess -> a
  // VkPipeline bound only by a push-constant BDA-address array, no
  // descriptor sets), caching by IR hash. Failure here is the sole source of
  // truth for whether this device/driver can run the module -- unlike
  // Metal, no HostAssisted fallback is attempted: the target NVIDIA/Linux
  // hardware is expected to support this natively, so failure is reported
  // as Unsupported rather than silently degraded.
  bool ensure_pipeline(const std::string& ir_hash, const std::string& glsl_source, uint32_t binding_count,
                       std::string* error);
  // Creates or reuses a host-visible-coherent VkBuffer for `allocation`,
  // uploading its current bytes. Invalidated (recreated) on generation or
  // required-size mismatch, mirroring Metal's allocation_map policy.
  bool ensure_buffer(const core::Allocation& allocation, AllocationRecord** out, std::string* error);
  bool ensure_timeline_semaphore(std::string* error);
  bool ensure_task_ring_pipeline(std::string* error);
  bool create_task_ring_buffers(uint32_t task_count, TaskRingBuffers* out, std::string* error);
  void destroy_task_ring_buffers(TaskRingBuffers* buffers);
  // Single command buffer covering both B8 tiers: dispatches the Tier0
  // publish kernel (compiler::task_ring_vulkan_source()) over `order.size()`
  // tasks, inserts a vkCmdPipelineBarrier2 making the ring's write visible
  // to a transfer read, copies each published task's x/y/z window directly
  // into that task's indirect slot, inserts a second barrier2 making that
  // transfer write visible to indirect-command reads, then issues one
  // vkCmdDispatchIndirect per task against `compute_pipeline_` (bound with
  // `addresses` as its push-constant BDA array). This is the Tier1
  // conformance floor: dispatch sizing is read back from the GPU-published
  // Task ring, never from host-side TaskRecord.x/y/z (contrast Metal, where
  // Tier1/ICB remains a target rather than a requirement -- see ADR-021 vs
  // ADR-022).
  bool dispatch_task_ring_and_tier1(const TaskRingBuffers& buffers, const std::vector<uint32_t>& order,
                                    const std::vector<VkDeviceAddress>& addresses, std::string* error);
  // Records and submits a single dispatch on a transient command buffer.
  // wait_value/signal_value of 0 mean "no timeline involvement for this
  // side" -- their fields are omitted from VkTimelineSemaphoreSubmitInfo
  // entirely rather than submitted as a literal 0, matching core's guarantee
  // that a required_value of 0 is rejected before reaching the backend.
  // VkFence + vkWaitForFences remains the host-side completion mechanism;
  // the timeline semaphore is a GPU-ordering primitive layered on top of it,
  // not a replacement for it in this v1 backend.
  bool dispatch_and_wait(const std::vector<VkDeviceAddress>& addresses, uint64_t wait_value, uint64_t signal_value,
                        std::string* error);
#endif

  friend std::unique_ptr<DeviceHal> make_device_hal(std::string* error);
};

std::unique_ptr<DeviceHal> make_device_hal(std::string* error = nullptr);

}  // namespace vg::vulkan

#endif
