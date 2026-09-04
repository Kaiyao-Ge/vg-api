#include "backends/metal/metal_device_internal.h"
#include "compiler/compute_task_ring.h"
#include <algorithm>
#include <chrono>

namespace vg::metal {

bool DeviceHal::Impl::ensure_timeline_event(std::string* error) {
  if (timeline_event != nil) return true;
  if (!snapshot.supports_shared_events) {
    if (error) *error = "device does not support MTLSharedEvent";
    return false;
  }
  timeline_event = [device newSharedEvent];
  if (timeline_event == nil) {
    if (error) *error = "failed to create MTLSharedEvent";
    return false;
  }
  return true;
}

bool DeviceHal::Impl::dispatch_and_wait(const std::vector<id<MTLBuffer>>& buffers, const std::vector<core::TaskRecord>& tasks,
                      core::TimelineGate gate, DispatchStats* stats, std::string* error) const {
  const auto encode_start = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  if (gate.wait != 0) [command_buffer encodeWaitForEvent:timeline_event value:gate.wait];
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:pipeline];
  for (size_t index = 0; index < buffers.size(); ++index) [encoder setBuffer:buffers[index] offset:0 atIndex:index];
  if (tasks.empty()) {
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  } else {
    for (const auto& task : tasks)
      [encoder dispatchThreadgroups:MTLSizeMake(task.x, task.y, task.z) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  }
  [encoder endEncoding];
  if (gate.signal != 0) [command_buffer encodeSignalEvent:timeline_event value:gate.signal];
  const auto submit_start = std::chrono::steady_clock::now();
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  const auto submit_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->cpu_encode_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
    stats->cpu_submit_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
    stats->encoder_count += 1;
    stats->command_buffer_count += 1;
    stats->queue_wait_count += 1;
  }
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal command buffer failed";
    return false;
  }
  return true;
}

bool DeviceHal::Impl::dispatch_indexed_and_wait(const std::vector<id<MTLBuffer>>& object_buffers, core::TimelineGate gate,
                              DispatchStats* stats, std::string* error) const {
  const auto encode_start = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  if (gate.wait != 0) [command_buffer encodeWaitForEvent:timeline_event value:gate.wait];

  const size_t table_bytes = std::max<size_t>(object_buffers.size() * sizeof(uint64_t), sizeof(uint64_t));
  id<MTLBuffer> table_buffer = [device newBufferWithLength:table_bytes options:MTLResourceStorageModeShared];
  if (table_buffer == nil) { if (error) *error = "failed to allocate indexed binding table buffer"; return false; }
  auto* table = static_cast<uint64_t*>([table_buffer contents]);
  for (size_t index = 0; index < object_buffers.size(); ++index) table[index] = [object_buffers[index] gpuAddress];

  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:pipeline];
  for (id<MTLBuffer> buffer : object_buffers)
    [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
  [encoder setBuffer:table_buffer offset:0 atIndex:0];
  [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];
  if (gate.signal != 0) [command_buffer encodeSignalEvent:timeline_event value:gate.signal];
  const auto submit_start = std::chrono::steady_clock::now();
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  const auto submit_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->cpu_encode_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
    stats->cpu_submit_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
    stats->encoder_count += 1;
    stats->command_buffer_count += 1;
    stats->queue_wait_count += 1;
  }
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal indexed command buffer failed";
    return false;
  }
  return true;
}

bool DeviceHal::Impl::dispatch_compute_task(id<MTLComputePipelineState> pipeline,
                           const std::vector<id<MTLBuffer>>& buffers,
                           const core::TaskRecord& task, uint32_t task_index,
                           uint32_t pipeline_ordinal, bool* submitted,
                           DispatchStats* stats,
                           std::string* error) const {
  if (submitted != nullptr) *submitted = false;
  if (pipeline == nil) {
    if (error) *error = "Metal schedule step has no per-Node compute pipeline";
    return false;
  }
  const auto encode_start = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:pipeline];
  for (size_t index = 0; index < buffers.size(); ++index)
    [encoder setBuffer:buffers[index] offset:0 atIndex:index];
  last_node_aware_dispatches.push_back(
      {task_index, task.node_index, task.node_generation,
       {task.x, task.y, task.z}, pipeline_ordinal});
  [encoder dispatchThreadgroups:MTLSizeMake(task.x, task.y, task.z)
           threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];

  const auto submit_start = std::chrono::steady_clock::now();
  [command_buffer commit];
  if (submitted != nullptr) *submitted = true;
  [command_buffer waitUntilCompleted];
  const auto submit_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->cpu_encode_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
    stats->cpu_submit_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
    stats->encoder_count += 1;
    stats->command_buffer_count += 1;
    stats->queue_wait_count += 1;
  }
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal scheduled compute command buffer failed";
    return false;
  }
  return true;
}

bool DeviceHal::Impl::dispatch_task_publish(TaskRingBuffers buffers, uint32_t count, DispatchStats* stats, std::string* error) const {
  const auto encode_start = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:task_ring_pipeline];
  [encoder setBuffer:buffers.state offset:0 atIndex:0];
  [encoder setBuffer:buffers.fields offset:0 atIndex:1];
  [encoder setBuffer:buffers.inputs offset:0 atIndex:2];
  [encoder dispatchThreadgroups:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];
  const auto submit_start = std::chrono::steady_clock::now();
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  const auto submit_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->cpu_encode_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
    stats->cpu_submit_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
    stats->encoder_count += 1;
    stats->command_buffer_count += 1;
    stats->queue_wait_count += 1;
  }
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal task ring dispatch failed";
    return false;
  }
  return true;
}

bool DeviceHal::Impl::dispatch_task_tier1_indirect(const std::vector<id<MTLBuffer>>& buffers, id<MTLBuffer> fields_buffer,
                                  const std::vector<uint32_t>& order, id<MTLBuffer> indirect_args_buffer,
                                  DispatchStats* stats, std::string* error) const {
  const auto encode_start = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  if (command_buffer == nil) { if (error) *error = "failed to create Metal command buffer"; return false; }
  const size_t stride = sizeof(MTLDispatchThreadgroupsIndirectArguments);
  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  if (blit == nil) { if (error) *error = "failed to create Metal blit encoder"; return false; }
  for (size_t i = 0; i < order.size(); ++i) {
    const size_t src_offset =
        (static_cast<size_t>(order[i]) * compiler::kTaskRingWordsPerRecord +
         compiler::kTaskRingDispatchXWord) * sizeof(uint32_t);
    [blit copyFromBuffer:fields_buffer sourceOffset:src_offset
                toBuffer:indirect_args_buffer destinationOffset:i * stride
                   size:3 * sizeof(uint32_t)];
  }
  [blit endEncoding];
  id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
  if (encoder == nil) { if (error) *error = "failed to create Metal compute encoder"; return false; }
  [encoder setComputePipelineState:pipeline];
  for (size_t index = 0; index < buffers.size(); ++index) [encoder setBuffer:buffers[index] offset:0 atIndex:index];
  for (size_t i = 0; i < order.size(); ++i)
    [encoder dispatchThreadgroupsWithIndirectBuffer:indirect_args_buffer
                                indirectBufferOffset:i * stride
                              threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];
  const auto submit_start = std::chrono::steady_clock::now();
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  const auto submit_end = std::chrono::steady_clock::now();
  if (stats != nullptr) {
    stats->cpu_encode_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_start - encode_start).count();
    stats->cpu_submit_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(submit_end - submit_start).count();
    stats->encoder_count += 2;
    stats->command_buffer_count += 1;
    stats->queue_wait_count += 1;
  }
  if (command_buffer.status == MTLCommandBufferStatusError || command_buffer.error != nil) {
    if (error)
      *error = command_buffer.error != nil ? [[command_buffer.error localizedDescription] UTF8String]
                                            : "Metal Tier1 indirect dispatch failed";
    return false;
  }
  return true;
}

}  // namespace vg::metal
