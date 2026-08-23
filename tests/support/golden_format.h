#ifndef VG_TESTS_SUPPORT_GOLDEN_FORMAT_H_
#define VG_TESTS_SUPPORT_GOLDEN_FORMAT_H_
#include "compiler/compiler.h"
#include <sstream>
#include <string>

// Shared between tools/vg-golden-gen (writes golden files) and
// tests/fixtures/compute_package_golden_test.cpp (diffs against them), so
// the two can never silently drift in how they render a ComputePackage.
namespace vg::golden {

inline const char* const kFixtureNames[] = {"load_only", "store_only", "atomic_add_only", "mixed"};

inline std::string format_source_map(const compiler::ComputePackage& package) {
  std::ostringstream out;
  for (const auto& entry : package.source_map)
    out << entry.instruction_index << '\t' << entry.generated_line << '\t' << entry.source << '\n';
  return out.str();
}

}  // namespace vg::golden
#endif
