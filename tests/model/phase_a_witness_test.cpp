#include "core/core.h"
#include <cassert>

int main() {
  vg::core::Certificate certificate;
  certificate.ranges.push_back({1, 0, 16, vg::ir::Access::Read, 2});
  vg::core::AccessWitness witness;
  witness.record({1, 4, 4, vg::ir::Access::Read, 2}, 0);
  witness.record({1, 8, 4, vg::ir::Access::Write, 2}, 1);
  witness.record({1, 12, 4, vg::ir::Access::Read, 3}, 2);
  auto diff = witness.diff(certificate);
  assert(diff.missing.size() == 2);
  assert(diff.unused.empty());

  vg::core::GraphEpochBuilder builder(11);
  assert(builder.add_reference({1, 3}));
  vg::core::GraphEpoch epoch;
  assert(builder.seal(&epoch));
  assert(epoch.sealed() && epoch.value() == 11);
  assert(epoch.contains({1, 3}));
  assert(!epoch.contains({1, 4}));
  return 0;
}
