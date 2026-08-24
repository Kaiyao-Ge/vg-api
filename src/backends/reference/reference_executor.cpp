#include "backends/reference/reference_executor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vg::reference {
namespace {
bool interpret_instruction(const ir::Instruction& instruction, size_t index, core::Arena& arena,
                           std::vector<core::PointerRef>& ref_values, bool* produced_output,
                           core::ExecutionResult* result) {
  const uint32_t generation = instruction.generation;
  core::Allocation* allocation = arena.lookup(core::RepresentationRef{instruction.allocation, generation, instruction.representation_epoch});
  if (allocation == nullptr || instruction.offset > allocation->size || instruction.size > allocation->size - instruction.offset) {
    result->ok = false; result->outputs_valid = false; result->poison = *produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned; result->message = "stale generation, representation epoch, or out-of-bounds allocation reference"; result->fault = {static_cast<uint32_t>(index), {instruction.allocation, instruction.offset, instruction.size, ir::Access::Read, instruction.representation_epoch}, "STALE_OR_BOUNDS", result->message, 0}; return false;
  }
  ir::Access access = ir::access_from_op(instruction.op);
  const ir::Effect effect{instruction.allocation, instruction.offset, instruction.size, access, instruction.representation_epoch};
  result->trace.push_back(effect);
  result->witness.record(effect, static_cast<uint32_t>(index));
  if (instruction.op == "load") return true;
  if (instruction.op == "load_ref") {
    if (instruction.size != sizeof(core::PointerRef::allocation) + sizeof(core::PointerRef::generation)) { result->ok = false; result->outputs_valid = false; result->poison = *produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned; result->message = "reference load_ref requires 12-byte width"; result->fault = {static_cast<uint32_t>(index), effect, "POINTER_REF_WIDTH", result->message, 0}; return false; }
    core::PointerRef ref{};
    std::memcpy(&ref.allocation, allocation->bytes.data() + instruction.offset, sizeof(ref.allocation));
    std::memcpy(&ref.generation, allocation->bytes.data() + instruction.offset + sizeof(ref.allocation), sizeof(ref.generation));
    ref_values[index] = ref;
    return true;
  }
  if (instruction.op == "load_via" || instruction.op == "store_via") {
    const core::PointerRef& ref = ref_values[instruction.ref_operand - 1];
    if (ref.allocation != instruction.allocation || ref.generation != instruction.generation) {
      result->ok = false; result->outputs_valid = false; result->poison = *produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned; result->message = "dereferenced pointer ref does not match dereferencing instruction's target"; result->fault = {static_cast<uint32_t>(index), effect, "DANGLING_POINTER_REF", result->message, 0}; return false;
    }
    if (instruction.op == "load_via") return true;
    for (uint64_t i = 0; i < instruction.size; ++i) allocation->bytes[static_cast<size_t>(instruction.offset + i)] = static_cast<uint8_t>(instruction.value);
    *produced_output = true;
    return true;
  }
  if (instruction.op == "store") {
    for (uint64_t i = 0; i < instruction.size; ++i) allocation->bytes[static_cast<size_t>(instruction.offset + i)] = static_cast<uint8_t>(instruction.value);
    *produced_output = true;
  } else if (instruction.op == "atomic_add") {
    if (instruction.size != sizeof(int64_t)) { result->ok = false; result->outputs_valid = false; result->poison = *produced_output ? core::PoisonState::PartiallyProduced : core::PoisonState::Poisoned; result->message = "reference atomic_add requires 8-byte width"; result->fault = {static_cast<uint32_t>(index), effect, "ATOMIC_WIDTH", result->message, 0}; return false; }
    int64_t value{}; std::memcpy(&value, allocation->bytes.data() + instruction.offset, sizeof(value)); value += instruction.value; std::memcpy(allocation->bytes.data() + instruction.offset, &value, sizeof(value));
    *produced_output = true;
  }
  return true;
}
}  // namespace

core::ExecutionResult execute(const ir::Module& module, core::Arena& arena, const core::Certificate* certificate,
                              core::Timeline* timeline, core::TimelineGate gate) {
  core::ExecutionResult result;
  result.ok = true;
  result.poison = core::PoisonState::Valid;
  const auto verification = ir::verify(module);
  if (!verification.ok) { result.ok = false; result.poison = core::PoisonState::Poisoned; result.message = verification.message; result.fault.code = "IR_INVALID"; result.fault.message = verification.message; return result; }
  if (timeline != nullptr && gate.wait != 0) {
    std::string wait_error;
    if (!timeline->validate_wait(gate.wait, &wait_error)) {
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
    if (!interpret_instruction(module.instructions[index], index, arena, ref_values, &produced_output, &result))
      return result;
  }
  if (timeline != nullptr && gate.signal != 0) {
    std::string signal_error;
    if (!timeline->signal(gate.signal, &signal_error)) {
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
  const auto count = static_cast<uint32_t>(tasks.size());
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

size_t texel_byte_offset(const core::CanonicalView& view, uint32_t layer, uint32_t level, uint32_t x, uint32_t y) {
  return static_cast<size_t>(view.subresource_byte_offset({layer, level}) +
                             static_cast<uint64_t>(y) * view.bytes_per_row(level) +
                             static_cast<uint64_t>(x) * core::bytes_per_texel(view.format));
}

std::array<float, 4> read_texel(const core::Allocation& allocation, const core::CanonicalView& view, uint32_t layer,
                                uint32_t level, uint32_t x, uint32_t y) {
  const size_t offset = texel_byte_offset(view, layer, level, x, y);
  if (offset + core::bytes_per_texel(view.format) > allocation.bytes.size()) return {0.0f, 0.0f, 0.0f, 1.0f};
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

void write_texel(core::Allocation& allocation, const core::CanonicalView& view, uint32_t layer, uint32_t level,
                 uint32_t x, uint32_t y, const std::array<float, 4>& rgba) {
  const size_t offset = texel_byte_offset(view, layer, level, x, y);
  if (offset + core::bytes_per_texel(view.format) > allocation.bytes.size()) return;
  if (view.format == core::PixelFormat::RGBA8Unorm) {
    for (int c = 0; c < 4; ++c) {
      const float clamped = std::min(1.0f, std::max(0.0f, rgba[static_cast<size_t>(c)]));
      allocation.bytes[offset + static_cast<size_t>(c)] =
          static_cast<uint8_t>(std::lround(clamped * 255.0f));
    }
    return;
  }
  const float value = rgba[0];
  std::memcpy(&allocation.bytes[offset], &value, sizeof(value));
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

const core::Allocation* resolve_view(const core::Arena& arena, const core::CanonicalView& view, const char* what,
                                     std::string* error) {
  std::string shape_error;
  if (!view.valid(&shape_error)) { *error = std::string(what) + ": " + shape_error; return nullptr; }
  const auto* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
  if (allocation == nullptr) { *error = std::string(what) + ": backing allocation not found"; return nullptr; }
  if (allocation->bytes.size() < view.byte_size()) {
    *error = std::string(what) + ": allocation holds " + std::to_string(allocation->bytes.size()) +
             " bytes but the CanonicalView's " + std::to_string(view.subresource_count()) +
             " subresources need " + std::to_string(view.byte_size());
    return nullptr;
  }
  return allocation;
}

bool check_subresource(const core::CanonicalView& view, uint32_t layer, uint32_t level, const char* what,
                       std::string* error) {
  if (layer >= view.array_layers) {
    *error = std::string(what) + ": array slice " + std::to_string(layer) +
             " is outside the CanonicalView's " + std::to_string(view.array_layers) + " layers";
    return false;
  }
  if (level >= view.mip_levels) {
    *error = std::string(what) + ": mip level " + std::to_string(level) +
             " is outside the CanonicalView's " + std::to_string(view.mip_levels) + " levels";
    return false;
  }
  return true;
}

std::array<float, 4> sample_level(const core::Allocation& allocation, const core::CanonicalView& view,
                                  core::FilterMode filter, core::WrapMode wrap, SampleCoord uv,
                                  uint32_t level) {
  const float u = uv.u;
  const float v = uv.v;
  const uint32_t layer = uv.array_slice;
  const uint32_t width = view.mip_width(level);
  const uint32_t height = view.mip_height(level);
  if (filter == core::FilterMode::Nearest) {
    const auto xi = static_cast<int32_t>(std::floor(u * static_cast<float>(width)));
    const auto yi = static_cast<int32_t>(std::floor(v * static_cast<float>(height)));
    return read_texel(allocation, view, layer, level, wrap_index(xi, width, wrap), wrap_index(yi, height, wrap));
  }
  const float px = u * static_cast<float>(width) - 0.5f;
  const float py = v * static_cast<float>(height) - 0.5f;
  const auto x0 = static_cast<int32_t>(std::floor(px));
  const auto y0 = static_cast<int32_t>(std::floor(py));
  const float fx = px - static_cast<float>(x0);
  const float fy = py - static_cast<float>(y0);
  const uint32_t xl = wrap_index(x0, width, wrap);
  const uint32_t xr = wrap_index(x0 + 1, width, wrap);
  const uint32_t yt = wrap_index(y0, height, wrap);
  const uint32_t yb = wrap_index(y0 + 1, height, wrap);
  const auto t00 = read_texel(allocation, view, layer, level, xl, yt);
  const auto t10 = read_texel(allocation, view, layer, level, xr, yt);
  const auto t01 = read_texel(allocation, view, layer, level, xl, yb);
  const auto t11 = read_texel(allocation, view, layer, level, xr, yb);
  std::array<float, 4> blended{};
  for (int c = 0; c < 4; ++c) {
    const float top = t00[static_cast<size_t>(c)] * (1.0f - fx) + t10[static_cast<size_t>(c)] * fx;
    const float bottom = t01[static_cast<size_t>(c)] * (1.0f - fx) + t11[static_cast<size_t>(c)] * fx;
    blended[static_cast<size_t>(c)] = top * (1.0f - fy) + bottom * fy;
  }
  return blended;
}

// `context` already names the caller and which input carries the lod/slice, so
// a diagnostic reads as a VG-level statement about that input (05 §14) instead
// of as a bare bounds complaint.
bool check_sample_subresource(const core::CanonicalView& view, const SampleCoord& coord,
                              const std::string& context, std::string* error) {
  if (coord.array_slice >= view.array_layers) {
    *error = context + " requests array slice " + std::to_string(coord.array_slice) +
             " but the CanonicalView declares " + std::to_string(view.array_layers) + " layers";
    return false;
  }
  const auto max_lod = static_cast<float>(view.mip_levels - 1);
  if (!(coord.lod >= 0.0f) || !(coord.lod <= max_lod)) {
    *error = context + " requests lod " + std::to_string(coord.lod) + " but the CanonicalView declares " +
             std::to_string(view.mip_levels) + " mip levels, so lod must be within [0, " +
             std::to_string(max_lod) + "]";
    return false;
  }
  return true;
}

// Nearest level selection is the GL/Vulkan rule ceil(lod + 0.5) - 1, so an
// exactly-half lod stays on the finer level rather than rounding up. Callers
// reach this only after check_sample_subresource has bounded the lod, which
// makes the level range provable; the clamp is a guard against a float edge
// case escaping that proof, not a policy -- an out-of-range lod is already a
// rejection by the time it gets here, never a clamp.
std::array<float, 4> sample_view(const core::Allocation& allocation, const core::CanonicalView& view,
                                 core::FilterMode filter, core::WrapMode wrap, const SampleCoord& coord) {
  std::array<float, 4> rgba{};
  if (filter == core::FilterMode::Nearest) {
    const float selected = std::ceil(coord.lod + 0.5f) - 1.0f;
    const uint32_t level =
        static_cast<uint32_t>(std::min(static_cast<float>(view.mip_levels - 1), std::max(0.0f, selected)));
    rgba = sample_level(allocation, view, filter, wrap, coord, level);
  } else {
    const float base = std::floor(coord.lod);
    const float fraction = coord.lod - base;
    const auto level = static_cast<uint32_t>(base);
    rgba = sample_level(allocation, view, filter, wrap, coord, level);
    if (fraction > 0.0f) {
      const auto coarse =
          sample_level(allocation, view, filter, wrap, coord, level + 1);
      for (int c = 0; c < 4; ++c)
        rgba[static_cast<size_t>(c)] =
            rgba[static_cast<size_t>(c)] * (1.0f - fraction) + coarse[static_cast<size_t>(c)] * fraction;
    }
  }
  if (!view.swizzle.identity()) rgba = apply_swizzle(view.swizzle, rgba);
  return rgba;
}

// The D3D/Metal standard sample positions, as offsets within the pixel. Any
// other count has no standard pattern, so raster/attachment passes refuse it
// instead of inventing one.
const std::vector<std::array<float, 2>>* standard_sample_positions(uint32_t sample_count) {
  static const std::vector<std::array<float, 2>> k1 = {{0.5f, 0.5f}};
  static const std::vector<std::array<float, 2>> k2 = {{0.25f, 0.25f}, {0.75f, 0.75f}};
  static const std::vector<std::array<float, 2>> k4 = {
      {0.375f, 0.125f}, {0.875f, 0.375f}, {0.125f, 0.625f}, {0.625f, 0.875f}};
  static const std::vector<std::array<float, 2>> k8 = {
      {0.5625f, 0.3125f}, {0.4375f, 0.6875f}, {0.8125f, 0.5625f}, {0.3125f, 0.1875f},
      {0.1875f, 0.8125f}, {0.0625f, 0.4375f}, {0.6875f, 0.9375f}, {0.9375f, 0.0625f}};
  switch (sample_count) {
    case 1: return &k1;
    case 2: return &k2;
    case 4: return &k4;
    case 8: return &k8;
    default: return nullptr;
  }
}

struct AttachmentPass {
  uint32_t width{};
  uint32_t height{};
  uint32_t sample_count{1};
  const std::vector<std::array<float, 2>>* sample_positions{};
  std::vector<std::array<float, 4>> samples;

  std::array<float, 4>& at(uint32_t x, uint32_t y, uint32_t sample) {
    return samples[(static_cast<size_t>(y) * width + x) * sample_count + sample];
  }
};

bool begin_attachment_pass(const core::Allocation& allocation, const core::CanonicalView& view,
                           const AttachmentFacetDesc& desc, const char* what, AttachmentPass* pass,
                           std::string* error) {
  if (!check_subresource(view, desc.subresource.layer, desc.subresource.level, what, error)) return false;
  pass->sample_positions = standard_sample_positions(desc.sample_count);
  if (pass->sample_positions == nullptr) {
    *error = std::string(what) + ": sample_count " + std::to_string(desc.sample_count) +
             " has no standard sample pattern; the attachment lowering supports 1, 2, 4 or 8";
    return false;
  }
  if (desc.sample_count > 1 && desc.store != AttachmentStoreAction::MultisampleResolve) {
    *error = std::string(what) + ": sample_count " + std::to_string(desc.sample_count) +
             " requires MultisampleResolve; a multisample attachment has no defined single-sample store";
    return false;
  }
  pass->width = view.mip_width(desc.subresource.level);
  pass->height = view.mip_height(desc.subresource.level);
  pass->sample_count = desc.sample_count;
  pass->samples.assign(static_cast<size_t>(pass->width) * pass->height * pass->sample_count,
                       std::array<float, 4>{});
  for (uint32_t y = 0; y < pass->height; ++y) {
    for (uint32_t x = 0; x < pass->width; ++x) {
      const std::array<float, 4> initial =
          desc.load == AttachmentLoadAction::Clear
              ? desc.clear_rgba
              : read_texel(allocation, view, desc.subresource.layer, desc.subresource.level, x, y);
      for (uint32_t s = 0; s < pass->sample_count; ++s) pass->at(x, y, s) = initial;
    }
  }
  return true;
}

void end_attachment_pass(core::Allocation& allocation, const core::CanonicalView& view,
                         const AttachmentFacetDesc& desc, AttachmentPass& pass,
                         std::vector<std::array<float, 4>>* resolved, bool* stored) {
  const float weight = 1.0f / static_cast<float>(pass.sample_count);
  resolved->assign(static_cast<size_t>(pass.width) * pass.height, std::array<float, 4>{});
  for (uint32_t y = 0; y < pass.height; ++y) {
    for (uint32_t x = 0; x < pass.width; ++x) {
      std::array<float, 4> average{};
      for (uint32_t s = 0; s < pass.sample_count; ++s) {
        const auto& sample = pass.at(x, y, s);
        for (int c = 0; c < 4; ++c) average[static_cast<size_t>(c)] += sample[static_cast<size_t>(c)] * weight;
      }
      (*resolved)[static_cast<size_t>(y) * pass.width + x] = average;
    }
  }
  *stored = desc.store != AttachmentStoreAction::DontCare;
  if (!*stored) return;
  for (uint32_t y = 0; y < pass.height; ++y)
    for (uint32_t x = 0; x < pass.width; ++x)
      write_texel(allocation, view, desc.subresource.layer, desc.subresource.level, x, y,
                  (*resolved)[static_cast<size_t>(y) * pass.width + x]);
  for (uint32_t y = 0; y < pass.height; ++y)
    for (uint32_t x = 0; x < pass.width; ++x)
      (*resolved)[static_cast<size_t>(y) * pass.width + x] =
          read_texel(allocation, view, desc.subresource.layer, desc.subresource.level, x, y);
}

float orient2d(float ax, float ay, float bx, float by, float px, float py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Top-left fill rule: a sample sitting exactly on an edge belongs to the
// triangle only when that edge is a top or a left edge. With winding normalized
// so orient2d over the three vertices is positive, those are the edges whose
// dy is negative, plus the horizontal edge running in +x.
struct EdgeDelta {
  float dx{};
  float dy{};
};

bool edge_covers(float value, EdgeDelta delta) {
  if (value > 0.0f) return true;
  if (value < 0.0f) return false;
  return delta.dy < 0.0f || (delta.dy == 0.0f && delta.dx > 0.0f);
}

// Sound over-approximation of the read set (docs/START.md §4 invariant 4): a
// fractional lod under bilinear filtering taps two adjacent levels, so the
// whole floor..ceil span counts as read even when the fraction is zero. Sharing
// an allocation is not by itself a conflict -- generating level 1 from level 0
// of one texture is a legitimate pass -- so only a real subresource collision is
// rejected.
bool raster_reads_its_target(const core::CanonicalView& source, const core::CanonicalView& target,
                             const RasterDesc& desc) {
  if (source.allocation != target.allocation) return false;
  if (source.allocation_generation != target.allocation_generation) return false;
  if (desc.source_array_slice != desc.attachment.subresource.layer) return false;
  const auto first = static_cast<uint32_t>(std::floor(desc.source_lod));
  const auto last = static_cast<uint32_t>(std::ceil(desc.source_lod));
  return desc.attachment.subresource.level >= first && desc.attachment.subresource.level <= last;
}
}  // namespace

SampleFacetResult sample_facet(const core::Arena& arena, const core::CanonicalView& view, core::FilterMode filter,
                               core::WrapMode wrap, const std::vector<SampleCoord>& coords) {
  SampleFacetResult result;
  const auto* allocation = resolve_view(arena, view, "sample facet", &result.message);
  if (allocation == nullptr) return result;

  result.sampled_rgba.reserve(coords.size());
  for (size_t index = 0; index < coords.size(); ++index) {
    if (!check_sample_subresource(view, coords[index],
                                  "sample facet: coordinate " + std::to_string(index), &result.message)) {
      result.sampled_rgba.clear();
      return result;
    }
    result.sampled_rgba.push_back(sample_view(*allocation, view, filter, wrap, coords[index]));
  }
  result.ok = true;
  return result;
}

SampleFacetResult sample_facet(const core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                               core::FilterMode filter, core::WrapMode wrap,
                               const std::vector<SampleCoord>& coords) {
  SampleFacetResult result;
  core::FacetStatus status = core::FacetStatus::Ok;
  const core::FacetSlot* slot = pool.lookup(arena, ref, &status);
  if (slot == nullptr) { result.message = core::to_string(status); return result; }
  if (slot->kind != core::FacetKind::Sample) { result.message = "facet kind mismatch"; return result; }
  return sample_facet(arena, slot->view, filter, wrap, coords);
}

SampleFacetResult sample_facet(const core::Arena& arena, const core::CanonicalView& view, core::FilterMode filter,
                               core::WrapMode wrap, const std::vector<std::array<float, 2>>& uv_coords) {
  std::vector<SampleCoord> coords;
  coords.reserve(uv_coords.size());
  for (const auto& uv : uv_coords) coords.push_back(SampleCoord{uv[0], uv[1], 0.0f, 0});
  return sample_facet(arena, view, filter, wrap, coords);
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

StorageFacetResult storage_facet(core::Arena& arena, const core::CanonicalView& view, StorageTexel target,
                                 const std::array<float, 4>& rgba) {
  StorageFacetResult result;
  if (resolve_view(arena, view, "storage facet", &result.message) == nullptr) return result;
  if (!check_subresource(view, target.layer, target.level, "storage facet", &result.message)) return result;
  if (!view.swizzle.identity()) {
    result.message =
        "storage facet: the CanonicalView carries a non-identity swizzle, which reinterprets a shader read of "
        "the view and has no defined meaning for an image write; write through an identity-swizzle view or "
        "transform the representation explicitly (06 §6.2)";
    return result;
  }
  const uint32_t width = view.mip_width(target.level);
  const uint32_t height = view.mip_height(target.level);
  if (target.x >= width || target.y >= height) {
    result.message = "storage facet: texel (" + std::to_string(target.x) + ", " + std::to_string(target.y) +
                     ") is outside mip level " + std::to_string(target.level) + "'s extent " +
                     std::to_string(width) + "x" + std::to_string(height);
    return result;
  }
  auto* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
  write_texel(*allocation, view, target.layer, target.level, target.x, target.y, rgba);
  result.written_rgba = read_texel(*allocation, view, target.layer, target.level, target.x, target.y);
  result.ok = true;
  return result;
}

StorageFacetResult storage_facet(core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                                 StorageTexel target, const std::array<float, 4>& rgba) {
  StorageFacetResult result;
  core::FacetStatus status = core::FacetStatus::Ok;
  const core::FacetSlot* slot = pool.lookup(arena, ref, &status);
  if (slot == nullptr) { result.message = core::to_string(status); return result; }
  if (slot->kind != core::FacetKind::Storage) { result.message = "facet kind mismatch"; return result; }
  const core::CanonicalView view = slot->view;
  return storage_facet(arena, view, target, rgba);
}

AttachmentFacetResult attachment_facet(core::Arena& arena, const core::CanonicalView& view,
                                       const AttachmentFacetDesc& desc) {
  AttachmentFacetResult result;
  if (resolve_view(arena, view, "attachment facet", &result.message) == nullptr) return result;
  auto* allocation = arena.lookup(core::PointerRef{view.allocation, view.allocation_generation});
  AttachmentPass pass;
  if (!begin_attachment_pass(*allocation, view, desc, "attachment facet", &pass, &result.message)) return result;
  end_attachment_pass(*allocation, view, desc, pass, &result.resolved_rgba, &result.stored);
  result.width = pass.width;
  result.height = pass.height;
  result.sample_count = pass.sample_count;
  result.contents_defined =
      desc.load != AttachmentLoadAction::DontCare && desc.store != AttachmentStoreAction::DontCare;
  result.ok = true;
  return result;
}

AttachmentFacetResult attachment_facet(core::Arena& arena, const core::FacetPool& pool, core::FacetRef ref,
                                       const AttachmentFacetDesc& desc) {
  AttachmentFacetResult result;
  core::FacetStatus status = core::FacetStatus::Ok;
  const core::FacetSlot* slot = pool.lookup(arena, ref, &status);
  if (slot == nullptr) { result.message = core::to_string(status); return result; }
  if (slot->kind != core::FacetKind::Attachment) { result.message = "facet kind mismatch"; return result; }
  const core::CanonicalView view = slot->view;
  return attachment_facet(arena, view, desc);
}

RasterResult raster_triangles(core::Arena& arena, const core::CanonicalView& source,
                              const core::CanonicalView& target, const RasterDesc& desc,
                              const std::vector<RasterVertex>& vertices) {
  RasterResult result;
  if (vertices.size() % 3 != 0) {
    result.message = "raster: triangle list needs a vertex count that is a multiple of 3, got " +
                     std::to_string(vertices.size());
    return result;
  }
  const auto* source_allocation = resolve_view(arena, source, "raster source", &result.message);
  if (source_allocation == nullptr) return result;
  if (!check_sample_subresource(source, SampleCoord{.lod = desc.source_lod, .array_slice = desc.source_array_slice},
                                "raster source: RasterDesc", &result.message))
    return result;
  if (resolve_view(arena, target, "raster target", &result.message) == nullptr) return result;
  if (raster_reads_its_target(source, target, desc)) {
    result.message =
        "raster: the source subresource the pass samples is the attachment subresource it renders into; a pass "
        "that reads the surface it writes has no defined result, so the oracle refuses rather than produce an "
        "order-dependent image (02 §3.2). Sampling a different slice/level of the same allocation is allowed.";
    return result;
  }
  auto* target_allocation = arena.lookup(core::PointerRef{target.allocation, target.allocation_generation});
  AttachmentPass pass;
  if (!begin_attachment_pass(*target_allocation, target, desc.attachment, "raster target", &pass,
                             &result.message))
    return result;

  const auto width = static_cast<float>(pass.width);
  const auto height = static_cast<float>(pass.height);
  for (size_t base = 0; base < vertices.size(); base += 3) {
    std::array<float, 3> x{};
    std::array<float, 3> y{};
    std::array<float, 3> u{};
    std::array<float, 3> v{};
    for (size_t i = 0; i < 3; ++i) {
      const RasterVertex& vertex = vertices[base + i];
      x[i] = (vertex.x * 0.5f + 0.5f) * width;
      y[i] = (0.5f - vertex.y * 0.5f) * height;
      u[i] = vertex.u;
      v[i] = vertex.v;
    }
    float area = orient2d(x[0], y[0], x[1], y[1], x[2], y[2]);
    if (area == 0.0f) continue;
    if (area < 0.0f) {
      std::swap(x[1], x[2]);
      std::swap(y[1], y[2]);
      std::swap(u[1], u[2]);
      std::swap(v[1], v[2]);
      area = -area;
    }
    const float min_x = std::min({x[0], x[1], x[2]});
    const float max_x = std::max({x[0], x[1], x[2]});
    const float min_y = std::min({y[0], y[1], y[2]});
    const float max_y = std::max({y[0], y[1], y[2]});
    const int64_t x_begin = std::max<int64_t>(0, static_cast<int64_t>(std::floor(min_x)) - 1);
    const int64_t x_end = std::min<int64_t>(static_cast<int64_t>(pass.width) - 1,
                                            static_cast<int64_t>(std::ceil(max_x)) + 1);
    const int64_t y_begin = std::max<int64_t>(0, static_cast<int64_t>(std::floor(min_y)) - 1);
    const int64_t y_end = std::min<int64_t>(static_cast<int64_t>(pass.height) - 1,
                                            static_cast<int64_t>(std::ceil(max_y)) + 1);
    for (int64_t iy = y_begin; iy <= y_end; ++iy) {
      for (int64_t ix = x_begin; ix <= x_end; ++ix) {
        for (uint32_t s = 0; s < pass.sample_count; ++s) {
          const auto& offset = (*pass.sample_positions)[s];
          const float sx = static_cast<float>(ix) + offset[0];
          const float sy = static_cast<float>(iy) + offset[1];
          const float e01 = orient2d(x[0], y[0], x[1], y[1], sx, sy);
          const float e12 = orient2d(x[1], y[1], x[2], y[2], sx, sy);
          const float e20 = orient2d(x[2], y[2], x[0], y[0], sx, sy);
          if (!edge_covers(e01, {.dx = x[1] - x[0], .dy = y[1] - y[0]})) continue;
          if (!edge_covers(e12, {.dx = x[2] - x[1], .dy = y[2] - y[1]})) continue;
          if (!edge_covers(e20, {.dx = x[0] - x[2], .dy = y[0] - y[2]})) continue;
          const float w0 = e12 / area;
          const float w1 = e20 / area;
          const float w2 = e01 / area;
          const SampleCoord coord{w0 * u[0] + w1 * u[1] + w2 * u[2], w0 * v[0] + w1 * v[1] + w2 * v[2],
                                  desc.source_lod, desc.source_array_slice};
          const auto sampled = sample_view(*source_allocation, source, desc.filter, desc.wrap, coord);
          auto& destination = pass.at(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy), s);
          for (int c = 0; c < 4; ++c)
            destination[static_cast<size_t>(c)] =
                sampled[static_cast<size_t>(c)] * desc.tint[static_cast<size_t>(c)];
          ++result.covered_fragment_count;
        }
      }
    }
  }

  end_attachment_pass(*target_allocation, target, desc.attachment, pass, &result.resolved_rgba, &result.stored);
  result.width = pass.width;
  result.height = pass.height;
  result.sample_count = pass.sample_count;
  result.contents_defined = desc.attachment.load != AttachmentLoadAction::DontCare &&
                            desc.attachment.store != AttachmentStoreAction::DontCare;
  result.ok = true;
  return result;
}

RasterResult raster_triangles(core::Arena& arena, const core::FacetPool& pool, core::RasterFacetPair facets,
                              const RasterDesc& desc,
                              const std::vector<RasterVertex>& vertices) {
  RasterResult result;
  core::FacetStatus status = core::FacetStatus::Ok;
  const core::FacetSlot* source_slot = pool.lookup(arena, facets.source, &status);
  if (source_slot == nullptr) { result.message = std::string("raster source: ") + core::to_string(status); return result; }
  if (source_slot->kind != core::FacetKind::Sample) { result.message = "raster source: facet kind mismatch"; return result; }
  const core::CanonicalView source = source_slot->view;
  const core::FacetSlot* target_slot = pool.lookup(arena, facets.target, &status);
  if (target_slot == nullptr) { result.message = std::string("raster target: ") + core::to_string(status); return result; }
  if (target_slot->kind != core::FacetKind::Attachment) { result.message = "raster target: facet kind mismatch"; return result; }
  const core::CanonicalView target = target_slot->view;
  return raster_triangles(arena, source, target, desc, vertices);
}
}
