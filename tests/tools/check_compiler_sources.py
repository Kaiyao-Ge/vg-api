#!/usr/bin/env python3
"""Explicit G2 byte/field comparison; never updates checked-in golden fixtures.

Run --record before a source-only move and --compare afterwards against the
same artifact. The generated driver and executable stay in the build directory.
"""
import argparse
import hashlib
import pathlib
import subprocess

DRIVER = r'''
#include "compiler/compiler.h"
#include <cassert>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
using namespace vg;
void field(const std::string& value) { std::cout << std::quoted(value) << '\n'; }
template<class T> void number(T value) { std::cout << value << '\n'; }
template<class P> void common(const P& p) {
  number(p.version); field(p.canonical_ir_hash); field(p.root_schema);
  number(p.source_map.size());
  for (const auto& s : p.source_map) {
    number(s.instruction_index); number(s.generated_line); field(s.source);
  }
  field(p.metal_source); field(p.vulkan_glsl_source);
}
void package(const compiler::ComputePackageResult& r) {
  number(r.ok); field(r.message); common(r.package);
  number(r.package.bindings.size());
  for (auto b : r.package.bindings) { number(b.allocation); number(b.binding); }
}
void package(const compiler::IndexedComputePackageResult& r) {
  number(r.ok); field(r.message); common(r.package);
  number(r.package.referenced_allocations.size());
  for (auto allocation : r.package.referenced_allocations) number(allocation);
  number(r.package.binding.table_binding); number(r.package.binding.stride);
  number(r.package.binding.count);
}
ir::Module module(std::vector<ir::Instruction> instructions,
                  std::vector<ir::PointerEdge> edges = {}) {
  ir::Module m; m.root_schema = "g2-root"; m.instructions = std::move(instructions);
  m.declared_pointer_edges = std::move(edges);
  for (size_t i = 0; i < m.instructions.size(); ++i) {
    auto& op = m.instructions[i]; op.source = "g2-source:" + std::to_string(i) + "\nquoted \"span\"";
    m.declared_effects.push_back({op.allocation, op.offset, op.size,
                                ir::access_from_op(op.op), op.representation_epoch});
  }
  return m;
}
int main() {
  using namespace compiler;
  const std::pair<const char*, std::string(*)()> sources[] = {
    {"task_ring_metal", task_ring_metal_source}, {"task_ring_vulkan", task_ring_vulkan_source},
    {"cull_compact_metal", cull_compact_metal_source}, {"cull_compact_vulkan", cull_compact_vulkan_source},
    {"sample_facet_metal", sample_facet_metal_source}, {"sample_facet_vulkan", sample_facet_vulkan_source},
    {"sample_facet_array_metal", sample_facet_array_metal_source},
    {"sample_facet_array_vulkan", sample_facet_array_vulkan_source},
    {"raster_facet_metal", raster_facet_metal_source}, {"raster_facet_vulkan", raster_facet_vulkan_source}
  };
  for (auto [name, emit] : sources) { field(name); field(emit()); }
  // Non-sorted/repeated allocation ids distinguish sorted direct bindings from
  // first-seen indexed table order. Signed stores cover byte-broadcast emission.
  auto linear = module({{"store", 9, 4, 4, -3, 1}, {"load", 2, 0, 4, 0, 1},
                        {"store", 9, 12, 4, 511, 1}});
  auto atomic = module({{"atomic_add", 7, 8, 8, -1, 1}, {"load", 7, 0, 4, 0, 1},
                        {"store", 7, 4, 4, 17, 1}});
  auto pointer = module({{"load_ref", 3, 0, 12, 0, 1},
                         {"store_via", 9, 4, 4, -7, 1, 0, 1},
                         {"load_via", 9, 0, 4, 0, 1, 0, 1}}, {{3, 0, 9}});
  auto supplied_hash = linear; supplied_hash.hash = "precomputed-canonical-hash";
  struct Case { std::string name; ir::Module input; bool linear_ok, pointer_ok, indexed_ok; };
  std::vector<Case> cases = {
    {"linear", linear, true, false, true}, {"atomic", atomic, true, false, false},
    {"pointer", pointer, false, true, false},
    {"supplied_hash", supplied_hash, true, false, true},
    {"load_ref_only", module({{"load_ref", 3, 0, 12, 0, 1}}), false, true, false},
    {"publish", module({{"publish", 3, 0, 4, 0, 1}}), false, false, false},
    {"load_size", module({{"load", 3, 0, 8, 0, 1}}), false, false, false},
    {"load_alignment", module({{"load", 3, 1, 4, 0, 1}}), false, false, false},
    {"atomic_size", module({{"atomic_add", 3, 0, 4, 0, 1}}), false, false, false},
    {"atomic_alignment", module({{"atomic_add", 3, 4, 8, 0, 1}}), false, false, false},
    {"ref_size", module({{"load_ref", 3, 0, 8, 0, 1}}), false, false, false},
    {"ref_alignment", module({{"load_ref", 3, 1, 12, 0, 1}}), false, false, false},
  };
  auto via_size = pointer; via_size.instructions[1].size = 8; via_size.declared_effects[1].size = 8;
  cases.push_back({"via_size", via_size, false, false, false});
  auto via_alignment = pointer; via_alignment.instructions[1].offset = 1; via_alignment.declared_effects[1].offset = 1;
  cases.push_back({"via_alignment", via_alignment, false, false, false});
  auto missing_effect = linear; missing_effect.declared_effects.clear();
  cases.push_back({"missing_effect", missing_effect, false, false, false});
  auto bad_version = linear; bad_version.version = 99;
  cases.push_back({"bad_version", bad_version, false, false, false});
  auto missing_edge = pointer; missing_edge.declared_pointer_edges.clear();
  cases.push_back({"missing_edge", missing_edge, false, false, false});
  for (const auto& c : cases) {
    field(c.name);
    auto l = build_linear_compute_package(c.input);
    auto p = build_pointer_graph_compute_package(c.input);
    auto i = build_indexed_compute_package(c.input);
    assert(l.ok == c.linear_ok && p.ok == c.pointer_ok && i.ok == c.indexed_ok);
    package(l); package(p); package(i);
  }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=pathlib.Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--record", type=pathlib.Path)
    mode.add_argument("--compare", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    cache = dict(line.split("=", 1) for line in (build / "CMakeCache.txt").read_text().splitlines()
                 if "=" in line and not line.startswith(("#", "//")))
    compiler = cache["CMAKE_CXX_COMPILER:FILEPATH"]
    driver = build / "g2-source-driver.cpp"
    binary = build / "g2-source-driver"
    driver.write_text(DRIVER)
    command = [compiler, "-std=c++20", "-I" + str(root / "src"), str(driver),
               str(build / "libvg_compiler.a"), str(build / "libvg_ir.a"), "-o", str(binary)]
    if cache.get("VG_ENABLE_SANITIZERS:BOOL") == "ON":
        command.append("-fsanitize=address,undefined")
    subprocess.run(command, check=True)
    output = subprocess.run([str(binary)], check=True, stdout=subprocess.PIPE).stdout
    digest = hashlib.sha256(output).hexdigest()
    if args.record:
        with args.record.open("xb") as target:
            target.write(output)
        print(f"Recorded {len(output)} bytes, SHA-256 {digest}")
    else:
        if output != args.compare.read_bytes():
            raise SystemExit("Compiler source/package baseline differs")
        print(f"Identical: {len(output)} bytes, SHA-256 {digest}")
    print("Coverage: 10 source functions (including both facet guards); 17 inputs x 3 package builders")


if __name__ == "__main__":
    main()
