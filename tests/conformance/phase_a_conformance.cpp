#include "backends/reference/reference_executor.h"
#include "capture/capture.h"
#include "compiler/compiler.h"
#include <cassert>
#include <cstring>

int main() {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,8,3) atomic_add(1,8,8,4) publish(1,0,1)");
  assert(compiled.ok);
  vg::core::Arena arena;
  auto& allocation = arena.allocate(32);
  std::memset(allocation.bytes.data(), 0, allocation.bytes.size());
  vg::core::Certificate certificate;
  certificate.ranges = compiled.module.declared_effects;
  auto result = vg::reference::execute(compiled.module, arena, &certificate);
  assert(result.ok && result.poison == vg::core::PoisonState::Valid);
  assert(result.outputs_valid && result.witness.entries().size() == compiled.module.instructions.size());
  int64_t value{};
  std::memcpy(&value, allocation.bytes.data() + 8, sizeof(value));
  assert(value == 4);

  const auto capture = vg::capture::serialize(compiled.module, arena);
  vg::ir::Module replayed;
  std::string error;
  assert(vg::capture::deserialize(capture, &replayed, &error));
  assert(replayed.hash == compiled.module.hash);

  auto rich_capture = vg::capture::make_capture(compiled.module, arena);
  rich_capture.certificate = certificate;
  rich_capture.execution = result;
  rich_capture.witness = result.witness;
  rich_capture.has_execution = true;
  rich_capture.graph_epoch = arena.topology_epoch();
  rich_capture.graph_references.push_back({allocation.id, allocation.generation});
  rich_capture.timeline_value = 3;
  rich_capture.source_hash = "source-fixture";
  const auto rich_text = vg::capture::serialize(rich_capture);
  vg::capture::Capture decoded_capture;
  assert(vg::capture::deserialize(rich_text, &decoded_capture, &error));
  assert(decoded_capture.graph_epoch == rich_capture.graph_epoch);
  assert(decoded_capture.witness.entries().size() == result.witness.entries().size());
  vg::capture::ReplayResult replay;
  assert(vg::capture::replay(decoded_capture, &replay, &error));
  assert(replay.execution.ok && replay.execution.witness.entries().size() == compiled.module.instructions.size());
  assert(replay.relocation.at(allocation.id) == allocation.id);

  vg::core::Certificate incomplete;
  incomplete.ranges.push_back(compiled.module.declared_effects.front());
  auto rejected = vg::reference::execute(compiled.module, arena, &incomplete);
  assert(!rejected.ok && rejected.poison == vg::core::PoisonState::Poisoned);
  assert(!rejected.outputs_valid && rejected.witness.entries().empty());
  assert(rejected.fault.code == "CERTIFICATE_MISS");
  assert(!rejected.missing_effects.empty());

  auto partial_module = vg::compiler::compile_c_like(
      "@node @effects store(1,16,4,9) atomic_add(1,20,4,1)");
  assert(partial_module.ok);
  vg::core::Certificate partial_certificate;
  partial_certificate.ranges = partial_module.module.declared_effects;
  auto partial = vg::reference::execute(partial_module.module, arena, &partial_certificate);
  assert(!partial.ok && partial.poison == vg::core::PoisonState::PartiallyProduced);
  assert(!partial.outputs_valid && partial.fault.code == "ATOMIC_WIDTH");
  assert(allocation.bytes[16] == 9);
  auto fault_capture = vg::capture::make_capture(partial_module.module, arena);
  fault_capture.certificate = partial_certificate;
  fault_capture.witness = partial.witness;
  fault_capture.execution = partial;
  fault_capture.has_execution = true;
  vg::capture::Capture decoded_fault;
  assert(vg::capture::deserialize(vg::capture::serialize(fault_capture), &decoded_fault, &error));
  vg::capture::ReplayResult replay_fault;
  assert(vg::capture::replay(decoded_fault, &replay_fault, &error));
  assert(!replay_fault.execution.ok && replay_fault.execution.poison == vg::core::PoisonState::PartiallyProduced);

  assert(arena.transform(allocation.id, allocation.generation, nullptr));
  auto epoch_mismatch = vg::reference::execute(compiled.module, arena);
  assert(!epoch_mismatch.ok && epoch_mismatch.poison == vg::core::PoisonState::Poisoned);
  assert(epoch_mismatch.fault.code == "STALE_OR_BOUNDS");

  auto& retired = arena.allocate(4);
  vg::ir::Module stale = compiled.module;
  stale.instructions[0].allocation = retired.id;
  stale.declared_effects[0].allocation = retired.id;
  assert(arena.retire(vg::core::PointerRef{retired.id, retired.generation}));
  auto stale_result = vg::reference::execute(stale, arena);
  assert(!stale_result.ok && stale_result.poison == vg::core::PoisonState::Poisoned);
  assert(stale_result.fault.code == "STALE_OR_BOUNDS");

  vg::core::AccessWitness witness;
  witness.record({allocation.id, 0, 4, vg::ir::Access::Read, 0}, 0);
  witness.record({allocation.id, 8, 4, vg::ir::Access::Write, 0}, 1);
  vg::core::Certificate witness_certificate;
  witness_certificate.ranges.push_back({allocation.id, 0, 4, vg::ir::Access::Read, 0});
  auto diff = witness.diff(witness_certificate);
  assert(diff.missing.size() == 1 && diff.unused.empty());
  return 0;
}
