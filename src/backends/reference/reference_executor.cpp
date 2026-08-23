#include "backends/reference/reference_executor.h"

#include <cmath>
#include <cstring>

namespace vg::reference {
core::ExecutionResult execute(const ir::Module& module, core::Arena& arena, const core::Certificate* certificate,
                              core::Timeline* timeline, uint64_t timeline_wait, uint64_t timeline_signal) {
  core::ExecutionResult result;
  result.ok = true;
  result.poison = core::PoisonState::Valid;
  const auto verification = ir::verify(module);
  if (!verification.ok) { result.ok = false; result.poison = core::PoisonState::Poisoned; result.message = verification.message; result.fault.code = "IR_INVALID"; result.fault.message = verification.message; return result; }
  if (timeline != nullptr && timeline_wait != 0) {
    std::string wait_error;
    if (!timeline->validate_wait(timeline_wait, &wait_error)) {
      result.ok = false; result.outputs_valid = false; result.poison = core::PoisonState::Poisoned;
      result.message = wait_error; result.fault.code = "TIMELINE_WAIT_UNSATISFIED"; result.fault.message = wait_error;
      return result;
    }
  }
  if (certificate != nullptr) {
    for (const auto& effect : verification.inferred_effects)
      if (!certificate->covers(effect)) result.missing_effects.push_back(effect);
    if (!result.missing_effects.empty()) {
      result.ok = false;
      result.outputs_valid = false;
      result.poison = core::PoisonState::Poisoned;
      result.message = "certificate does not cover inferred effect";
      result.fault = {0, result.missing_effects.front(), "CERTIFICATE_MISS", result.message, 0};
      return result;
    }
  }
  bool produced_output = false;
  std::vector<core::PointerRef> ref_values(module.instructions.size());
  for (size_t index = 0; index < module.instructions.size(); ++index) {
    const auto& instruction = module.instructions[index];
    const uint32_t generation = instruction.generation;
    core::Allocation* allocation = arena.lookup(instruction.allocation, generation,
                                                instruction.representation_epoch);
    if (allocation == nullptr || instruction.offset > allocation->size || instruction.size > allocation->size - instruction.offset) {
      result.ok = false; result.outputs_valid = false; result.poison = produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned; result.message = "stale generation, representation epoch, or out-of-bounds allocation reference"; result.fault = {static_cast<uint32_t>(index), {instruction.allocation, instruction.offset, instruction.size, ir::Access::Read, instruction.representation_epoch}, "STALE_OR_BOUNDS", result.message, 0}; return result;
    }
    ir::Access access = instruction.op == "load" ? ir::Access::Read : instruction.op == "store" ? ir::Access::Write : instruction.op == "atomic_add" ? ir::Access::Atomic : instruction.op == "publish" ? ir::Access::Publish : instruction.op == "load_ref" ? ir::Access::Read : instruction.op == "load_via" ? ir::Access::Read : instruction.op == "store_via" ? ir::Access::Write : ir::Access::Read;
    const ir::Effect effect{instruction.allocation, instruction.offset, instruction.size, access, instruction.representation_epoch};
    result.trace.push_back(effect);
    result.witness.record(effect, static_cast<uint32_t>(index));
    if (instruction.op == "load") continue;
    if (instruction.op == "load_ref") {
      if (instruction.size != sizeof(core::PointerRef::allocation) + sizeof(core::PointerRef::generation)) { result.ok = false; result.outputs_valid = false; result.poison = produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned; result.message = "reference load_ref requires 12-byte width"; result.fault = {static_cast<uint32_t>(index), effect, "POINTER_REF_WIDTH", result.message, 0}; return result; }
      core::PointerRef ref{};
      std::memcpy(&ref.allocation, allocation->bytes.data() + instruction.offset, sizeof(ref.allocation));
      std::memcpy(&ref.generation, allocation->bytes.data() + instruction.offset + sizeof(ref.allocation), sizeof(ref.generation));
      ref_values[index] = ref;
      continue;
    }
    if (instruction.op == "load_via" || instruction.op == "store_via") {
      const core::PointerRef& ref = ref_values[instruction.ref_operand - 1];
      if (ref.allocation != instruction.allocation || ref.generation != instruction.generation) {
        result.ok = false; result.outputs_valid = false; result.poison = produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned; result.message = "dereferenced pointer ref does not match dereferencing instruction's target"; result.fault = {static_cast<uint32_t>(index), effect, "DANGLING_POINTER_REF", result.message, 0}; return result;
      }
      if (instruction.op == "load_via") continue;
      for (uint64_t i = 0; i < instruction.size; ++i) allocation->bytes[static_cast<size_t>(instruction.offset + i)] = static_cast<uint8_t>(instruction.value);
      produced_output = true;
      continue;
    }
    if (instruction.op == "store") {
      for (uint64_t i = 0; i < instruction.size; ++i) allocation->bytes[static_cast<size_t>(instruction.offset + i)] = static_cast<uint8_t>(instruction.value);
      produced_output = true;
    } else if (instruction.op == "atomic_add") {
      if (instruction.size != sizeof(int64_t)) { result.ok = false; result.outputs_valid = false; result.poison = produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned; result.message = "reference atomic_add requires 8-byte width"; result.fault = {static_cast<uint32_t>(index), effect, "ATOMIC_WIDTH", result.message, 0}; return result; }
      int64_t value{}; std::memcpy(&value, allocation->bytes.data() + instruction.offset, sizeof(value)); value += instruction.value; std::memcpy(allocation->bytes.data() + instruction.offset, &value, sizeof(value));
      produced_output = true;
    }
  }
  if (timeline != nullptr && timeline_signal != 0) {
    std::string signal_error;
    if (!timeline->signal(timeline_signal, &signal_error)) {
      result.ok = false; result.outputs_valid = false; result.poison = produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned;
      result.message = signal_error; result.fault.code = "TIMELINE_SIGNAL_NOT_MONOTONIC"; result.fault.message = signal_error;
      return result;
    }
  }
  return result;
}

TaskGraphExecutionResult execute_task_graph(const core::TaskGraph& task_graph) {
  TaskGraphExecutionResult result;
  std::string error;
  if (!task_graph.validate_execution(&error)) { result.message = error; return result; }

  const auto& tasks = task_graph.tasks();
  const uint32_t count = static_cast<uint32_t>(tasks.size());
  std::vector<uint32_t> order;
  if (!task_graph.deterministic_order(&order, &error)) { result.message = error; return result; }

  core::PublicationRing ring(count);
  result.published_tasks.reserve(count);
  for (uint32_t index : order) {
    uint32_t slot = 0;
    if (!ring.publish_task(tasks[index], &slot, &error)) { result.message = error; return result; }
    if (!ring.consume(slot, &error)) { result.message = error; return result; }
    result.published_tasks.push_back(tasks[index]);
  }
  result.ok = true;
  return result;
}

CullCompactResult cull_compact(const std::vector<uint32_t>& instance_visible,
                               const std::vector<uint32_t>& instance_ids) {
  CullCompactResult result;
  if (instance_visible.size() != instance_ids.size()) {
    result.message = "instance_visible and instance_ids must be the same size";
    return result;
  }
  for (size_t i = 0; i < instance_visible.size(); ++i)
    if (instance_visible[i] != 0) result.compact_ids.push_back(instance_ids[i]);
  result.ok = true;
  return result;
}

namespace {
uint32_t wrap_index(int32_t index, uint32_t size, core::WrapMode wrap) {
  if (wrap == core::WrapMode::Repeat) {
    int32_t wrapped = index % static_cast<int32_t>(size);
    if (wrapped < 0) wrapped += static_cast<int32_t>(size);
    return static_cast<uint32_t>(wrapped);
  }
  if (index < 0) return 0;
  if (index >= static_cast<int32_t>(size)) return size - 1;
  return static_cast<uint32_t>(index);
}

std::array<float, 4> read_texel(const core::Allocation& allocation, const core::CanonicalView& view, uint32_t x,
                                uint32_t y) {
  constexpr size_t kBytesPerPixel = 4;
  const size_t offset = (static_cast<size_t>(y) * view.width + x) * kBytesPerPixel;
  if (offset + kBytesPerPixel > allocation.bytes.size()) return {0.0f, 0.0f, 0.0f, 1.0f};
  if (view.format == core::PixelFormat::RGBA8Unorm) {
    return {static_cast<float>(allocation.bytes[offset + 0]) / 255.0f,
            static_cast<float>(allocation.bytes[offset + 1]) / 255.0f,
            static_cast<float>(allocation.bytes[offset + 2]) / 255.0f,
            static_cast<float>(allocation.bytes[offset + 3]) / 255.0f};
  }
  float value{};
  std::memcpy(&value, &allocation.bytes[offset], sizeof(float));
  return {value, 0.0f, 0.0f, 1.0f};
}

// Channel selection is a pure per-component pick, so it commutes with linear
// filtering -- applying it once to the filtered result matches what a GPU
// swizzled texture view returns.
std::array<float, 4> apply_swizzle(const core::SwizzleChannels& swizzle, const std::array<float, 4>& rgba) {
  const auto pick = [&rgba](core::Swizzle channel) -> float {
    switch (channel) {
      case core::Swizzle::Red: return rgba[0];
      case core::Swizzle::Green: return rgba[1];
      case core::Swizzle::Blue: return rgba[2];
      case core::Swizzle::Alpha: return rgba[3];
      case core::Swizzle::Zero: return 0.0f;
      case core::Swizzle::One: return 1.0f;
    }
    return 0.0f;
  };
  return {pick(swizzle.red), pick(swizzle.green), pick(swizzle.blue), pick(swizzle.alpha)};
}
}  // namespace

SampleFacetResult sample_facet(const core::Arena& arena, const core::CanonicalView& view, core::FilterMode filter,
                               core::WrapMode wrap, const std::vector<std::array<float, 2>>& uv_coords) {
  SampleFacetResult result;
  const auto* allocation = arena.lookup(view.allocation, view.allocation_generation);
  if (allocation == nullptr) { result.message = "sample facet: backing allocation not found"; return result; }
  if (view.width == 0 || view.height == 0) { result.message = "sample facet: view has zero extent"; return result; }

  for (const auto& uv : uv_coords) {
    if (filter == core::FilterMode::Nearest) {
      const int32_t xi = static_cast<int32_t>(std::floor(uv[0] * static_cast<float>(view.width)));
      const int32_t yi = static_cast<int32_t>(std::floor(uv[1] * static_cast<float>(view.height)));
      result.sampled_rgba.push_back(
          read_texel(*allocation, view, wrap_index(xi, view.width, wrap), wrap_index(yi, view.height, wrap)));
      continue;
    }
    const float px = uv[0] * static_cast<float>(view.width) - 0.5f;
    const float py = uv[1] * static_cast<float>(view.height) - 0.5f;
    const int32_t x0 = static_cast<int32_t>(std::floor(px));
    const int32_t y0 = static_cast<int32_t>(std::floor(py));
    const float fx = px - static_cast<float>(x0);
    const float fy = py - static_cast<float>(y0);
    const auto t00 = read_texel(*allocation, view, wrap_index(x0, view.width, wrap), wrap_index(y0, view.height, wrap));
    const auto t10 = read_texel(*allocation, view, wrap_index(x0 + 1, view.width, wrap), wrap_index(y0, view.height, wrap));
    const auto t01 = read_texel(*allocation, view, wrap_index(x0, view.width, wrap), wrap_index(y0 + 1, view.height, wrap));
    const auto t11 = read_texel(*allocation, view, wrap_index(x0 + 1, view.width, wrap), wrap_index(y0 + 1, view.height, wrap));
    std::array<float, 4> blended{};
    for (int c = 0; c < 4; ++c) {
      const float top = t00[c] * (1.0f - fx) + t10[c] * fx;
      const float bottom = t01[c] * (1.0f - fx) + t11[c] * fx;
      blended[c] = top * (1.0f - fy) + bottom * fy;
    }
    result.sampled_rgba.push_back(blended);
  }
  if (!view.swizzle.identity())
    for (auto& rgba : result.sampled_rgba) rgba = apply_swizzle(view.swizzle, rgba);
  result.ok = true;
  return result;
}

SampleFacetResult sample_facet(const core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                               core::FilterMode filter, core::WrapMode wrap,
                               const std::vector<std::array<float, 2>>& uv_coords) {
  SampleFacetResult result;
  core::FacetStatus status = core::FacetStatus::Ok;
  const core::FacetSlot* slot = pool.lookup(arena, ref, &status);
  if (slot == nullptr) { result.message = core::to_string(status); return result; }
  if (slot->kind != core::FacetKind::Sample) { result.message = "facet kind mismatch"; return result; }
  return sample_facet(arena, slot->view, filter, wrap, uv_coords);
}
}
