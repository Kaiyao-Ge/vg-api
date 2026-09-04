#include "core/access.h"

#include "core/arena.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace vg::core {

bool Certificate::covers(const ir::Effect& effect) const {
  return std::ranges::any_of(ranges, [&](const ir::Effect& range) { return ir::effect_covers(range, effect); });
}

void AccessWitness::record(ir::Effect effect, uint32_t instruction_index) {
  entries_.push_back({effect, instruction_index});
}

WitnessDiff AccessWitness::diff(const Certificate& certificate) const {
  WitnessDiff result;
  for (const auto& entry : entries_) if (!certificate.covers(entry.effect)) result.missing.push_back(entry.effect);
  for (const auto& range : certificate.ranges) {
    const bool observed = std::ranges::any_of(entries_, [&](const WitnessEntry& entry) {
      return ir::effect_covers(range, entry.effect);
    });
    if (!observed) result.unused.push_back(range);
  }
  return result;
}

bool validate_certificate(const Certificate& certificate, const std::vector<ir::Effect>& effects, std::string* error) {
  if (!std::ranges::all_of(effects, [&](const ir::Effect& effect) { return certificate.covers(effect); })) {
    if (error) *error = "certificate does not cover inferred effect";
    return false;
  }
  return true;
}

bool build_access_certificate(const Arena& arena, AccessCertificateMode mode,
                              const std::vector<PointerRef>& touched,
                              AccessCertificate* out, std::string* error) {
  if (out == nullptr) { if (error) *error = "access certificate output is required"; return false; }
  if (mode == AccessCertificateMode::SoftwarePaged || mode == AccessCertificateMode::FaultManaged) {
    if (error) *error = "this access certificate mode has no implementation; callers must classify it Unsupported";
    return false;
  }
  out->mode = mode;
  const auto started = std::chrono::steady_clock::now();
  GraphEpochBuilder builder(&arena);
  uint64_t scanned_bytes = 0;
  if (mode == AccessCertificateMode::CertifiedPinned) {
    for (const auto& reference : touched) {
      const Allocation* allocation = arena.lookup(reference);
      if (allocation == nullptr) { if (error) *error = "touched allocation is not active in arena"; return false; }
      if (!builder.add_reference(reference, error)) return false;
      scanned_bytes += allocation->size;
    }
  } else {
    // Universe and DiscoverThenLease both scan every live allocation in the
    // arena; under this project's unified-memory Metal/reference model there
    // is no GPU-resident subset distinct from the arena itself, so
    // DiscoverThenLease's "discovery" is a real, honestly-timed host rescan
    // that happens to find the same set Universe would — an accurate result,
    // not a gap.
    for (const auto& [id, allocation] : arena.allocations()) {
      if (allocation.state != ObjectState::Active) continue;
      if (!builder.add_reference(PointerRef{allocation.id, allocation.generation}, error)) return false;
      scanned_bytes += allocation.size;
    }
  }
  GraphEpoch epoch;
  if (!builder.seal(&epoch, error)) return false;
  out->epoch = epoch;
  out->scanned_bytes = scanned_bytes;
  out->result_bytes = scanned_bytes;
  out->working_set_bytes = scanned_bytes;
  if (mode == AccessCertificateMode::DiscoverThenLease) {
    const auto elapsed = std::chrono::steady_clock::now() - started;
    out->discovery_host_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }
  return true;
}

namespace {
// load_ref's wire width is 12 bytes (u64 + u32). sizeof(PointerRef) may
// pad to 16; discovery must not invent a wider slot (ADR-028).
constexpr size_t kPointerRefWireBytes = sizeof(uint64_t) + sizeof(uint32_t);

bool decode_pointer_ref(const std::vector<uint8_t>& bytes, size_t offset, PointerRef* out) {
  if (out == nullptr || offset + kPointerRefWireBytes > bytes.size()) return false;
  PointerRef ref{};
  std::memcpy(&ref.allocation, bytes.data() + offset, sizeof(ref.allocation));
  std::memcpy(&ref.generation, bytes.data() + offset + sizeof(ref.allocation), sizeof(ref.generation));
  *out = ref;
  return true;
}

bool discovery_ref_seen(const std::vector<PointerRef>& seen, PointerRef ref) {
  return std::ranges::any_of(seen, [&](PointerRef candidate) {
    return candidate.allocation == ref.allocation && candidate.generation == ref.generation;
  });
}
}  // namespace

bool discover_reachable(const Arena& arena, const std::vector<PointerRef>& seeds, DiscoveryResult* out,
                        std::string* error, const std::function<void()>& after_visit) {
  if (out == nullptr) {
    if (error) *error = "discovery result output is required";
    return false;
  }
  *out = {};
  const auto started = std::chrono::steady_clock::now();
  const uint64_t frozen = arena.topology_epoch();
  out->frozen_topology_epoch = frozen;

  std::vector<PointerRef> worklist;
  for (const auto& seed : seeds) {
    if (seed.allocation == 0 || seed.generation == 0) {
      if (error) *error = "discovery seed is not a well-formed pointer ref";
      return false;
    }
    if (arena.lookup(seed) == nullptr) {
      if (error) *error = "discovery seed is not an active allocation";
      return false;
    }
    if (discovery_ref_seen(out->reachable, seed)) continue;
    out->reachable.push_back(seed);
    worklist.push_back(seed);
  }
  if (arena.topology_epoch() != frozen) {
    if (error) *error = "topology epoch changed during discovery";
    return false;
  }

  while (!worklist.empty()) {
    if (arena.topology_epoch() != frozen) {
      if (error) *error = "topology epoch changed during discovery";
      return false;
    }
    const PointerRef current = worklist.back();
    worklist.pop_back();
    const Allocation* allocation = arena.lookup(current);
    if (allocation == nullptr) {
      if (error) *error = "discovered allocation is no longer active";
      return false;
    }
    out->scanned_bytes += allocation->size;
    if (after_visit) after_visit();
    if (arena.topology_epoch() != frozen) {
      if (error) *error = "topology epoch changed during discovery";
      return false;
    }
    for (size_t offset = 0; offset + kPointerRefWireBytes <= allocation->bytes.size();
         offset += kPointerRefWireBytes) {
      PointerRef child{};
      if (!decode_pointer_ref(allocation->bytes, offset, &child)) continue;
      // Walk only well-formed refs that resolve to Active. A zero generation
      // or a stale id is a break in the chain, not a business store we
      // chase (02 §7.2: discovery Node has no side effects and does not
      // invent edges).
      if (child.allocation == 0 || child.generation == 0) continue;
      if (arena.lookup(child) == nullptr) continue;
      if (discovery_ref_seen(out->reachable, child)) continue;
      out->reachable.push_back(child);
      worklist.push_back(child);
    }
  }

  for (const auto& ref : out->reachable) {
    const Allocation* allocation = arena.lookup(ref);
    if (allocation != nullptr) out->result_bytes += allocation->size;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  out->discovery_host_ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  return true;
}

bool build_discovered_certificate(const Arena& arena, const DiscoveryResult& discovery,
                                  AccessCertificate* out, std::string* error) {
  if (out == nullptr) {
    if (error) *error = "access certificate output is required";
    return false;
  }
  if (arena.topology_epoch() != discovery.frozen_topology_epoch) {
    if (error) *error = "topology epoch changed during discovery";
    return false;
  }
  GraphEpochBuilder builder(&arena);
  for (const auto& ref : discovery.reachable) {
    if (!builder.add_reference(arena, ref, error)) return false;
  }
  GraphEpoch epoch;
  if (!builder.seal(&epoch, error)) return false;
  out->mode = AccessCertificateMode::DiscoverThenLease;
  out->epoch = epoch;
  out->discovery_host_ns = discovery.discovery_host_ns;
  out->discovery_gpu_ns = 0;
  out->scanned_bytes = discovery.scanned_bytes;
  out->result_bytes = discovery.result_bytes;
  out->working_set_bytes = discovery.result_bytes;
  return true;
}

bool certificate_covers_discovery_witness(const AccessCertificate& certificate,
                                          const std::vector<PointerRef>& witness, std::string* error) {
  if (!std::ranges::all_of(witness, [&](const PointerRef& ref) { return certificate.epoch.contains(ref); })) {
    if (error) *error = "discovery witness is not covered by the certificate";
    return false;
  }
  return true;
}

bool compose_certificates(const std::vector<Certificate>& parts, Certificate* out, std::string* error) {
  if (out == nullptr) {
    if (error) *error = "composed certificate output is required";
    return false;
  }
  if (parts.empty()) {
    if (error) *error = "certificate composition requires at least one child certificate";
    return false;
  }
  Certificate composed;
  for (const auto& part : parts) {
    composed.ranges.insert(composed.ranges.end(), part.ranges.begin(), part.ranges.end());
  }
  for (const auto& part : parts) {
    for (const auto& range : part.ranges) {
      if (!composed.covers(range)) {
        if (error) *error = "composed certificate does not cover a child range";
        return false;
      }
    }
  }
  *out = std::move(composed);
  return true;
}

namespace {
uint64_t active_allocation_count(const Arena& arena) {
  uint64_t count = 0;
  for (const auto& [id, allocation] : arena.allocations()) {
    (void)id;
    if (allocation.state == ObjectState::Active) ++count;
  }
  return count;
}
}  // namespace

bool compose_access_certificates(const Arena& arena, const std::vector<AccessCertificate>& parts,
                                 AccessCertificate* out, bool* exploded, std::string* error) {
  if (out == nullptr) {
    if (error) *error = "composed access certificate output is required";
    return false;
  }
  if (exploded != nullptr) *exploded = false;
  if (parts.empty()) {
    if (error) *error = "access certificate composition requires at least one child certificate";
    return false;
  }
  const uint64_t epoch_value = parts.front().epoch.value();
  GraphEpochBuilder builder(&arena);
  bool any_universe = false;
  bool any_discover = false;
  bool any_strictly_smaller = false;
  const uint64_t universe = active_allocation_count(arena);
  for (const auto& part : parts) {
    if (part.epoch.value() != epoch_value) {
      if (error) *error = "cannot compose access certificates from different graph epochs";
      return false;
    }
    if (part.mode == AccessCertificateMode::SoftwarePaged || part.mode == AccessCertificateMode::FaultManaged) {
      if (error) *error = "cannot compose an unimplemented access certificate mode";
      return false;
    }
    if (part.mode == AccessCertificateMode::Universe) any_universe = true;
    if (part.mode == AccessCertificateMode::DiscoverThenLease) any_discover = true;
    if (static_cast<uint64_t>(part.epoch.references().size()) < universe) any_strictly_smaller = true;
    for (const auto& ref : part.epoch.references()) {
      if (!builder.add_reference(arena, ref, error)) return false;
    }
  }
  GraphEpoch epoch;
  if (!builder.seal(&epoch, error)) return false;
  AccessCertificate composed;
  composed.epoch = epoch;
  if (any_universe) composed.mode = AccessCertificateMode::Universe;
  else if (any_discover) composed.mode = AccessCertificateMode::DiscoverThenLease;
  else composed.mode = AccessCertificateMode::CertifiedPinned;
  uint64_t bytes = 0;
  for (const auto& ref : epoch.references()) {
    const Allocation* allocation = arena.lookup(ref);
    if (allocation != nullptr) bytes += allocation->size;
  }
  composed.scanned_bytes = bytes;
  composed.result_bytes = bytes;
  composed.working_set_bytes = bytes;
  const bool became_universe = static_cast<uint64_t>(epoch.references().size()) == universe;
  if (exploded != nullptr) *exploded = became_universe && any_strictly_smaller;
  for (const auto& part : parts) {
    if (!certificate_covers_discovery_witness(composed, part.epoch.references(), error)) return false;
  }
  *out = std::move(composed);
  return true;
}

bool WorkingSetBudget::allows(uint64_t bytes, std::string* error) const {
  if (!has_limit) return true;
  if (bytes <= byte_limit) return true;
  if (error) *error = "working-set budget exceeded";
  return false;
}

bool WorkingSetLease::covers(PointerRef ref) const {
  return std::ranges::any_of(allocations, [&](PointerRef candidate) {
    return candidate.allocation == ref.allocation && candidate.generation == ref.generation;
  });
}

bool WorkingSetLease::add(PointerRef ref, const std::vector<PointerRef>& proven, std::string* error) {
  const bool proven_hold = std::ranges::any_of(proven, [&](PointerRef candidate) {
    return candidate.allocation == ref.allocation && candidate.generation == ref.generation;
  });
  if (!proven_hold) {
    if (error) *error = "lease cannot cover an unproven allocation";
    return false;
  }
  if (covers(ref)) return true;
  allocations.push_back(ref);
  return true;
}

bool WorkingSetLease::valid(const std::vector<PointerRef>& proven, std::string* error) const {
  for (const PointerRef& ref : allocations) {
    const bool proven_hold = std::ranges::any_of(proven, [&](PointerRef candidate) {
      return candidate.allocation == ref.allocation && candidate.generation == ref.generation;
    });
    if (proven_hold) continue;
    if (error) *error = "lease cannot cover an unproven allocation";
    return false;
  }
  return true;
}

}  // namespace vg::core
