#include "../support/assembled_plan_fixture.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/vulkan/vulkan_device_hal.h"
#include "compiler/compute_package.h"
#include "ir/ir.h"

#include <cstring>
#include <iostream>

namespace {
vg::ir::Module pointer_module(const vg::core::Allocation &holder,
                              const vg::core::Allocation &target) {
  vg::ir::Module m;
  m.version = 1;
  m.root_schema = "vg.test/vulkan-pointer-graph";
  m.instructions = {{"load_ref", holder.id, 0, 12, 0, holder.generation,
                     holder.representation_epoch, 0, ""},
                    {"store_via", target.id, 0, 4, 0x2a, target.generation,
                     target.representation_epoch, 1, ""}};
  m.declared_effects = {
      {holder.id, 0, 12, vg::ir::Access::Read, holder.representation_epoch}};
  m.declared_pointer_edges = {{holder.id, 0, target.id}};
  return m;
}

vg::ir::Module linear_store_module(const vg::core::Allocation &target) {
  vg::ir::Module m;
  m.version = 1;
  m.root_schema = "vg.test/vulkan-linear";
  m.instructions = {{"store", target.id, 0, 4, 7, target.generation,
                     target.representation_epoch, 0, ""}};
  m.declared_effects = {
      {target.id, 0, 4, vg::ir::Access::Write, target.representation_epoch}};
  return m;
}

bool cached(const vg::hal::CompiledPlan &compiled) {
  for (const auto &e : compiled.report.events)
    if (e.operation == "node_compute_package" &&
        e.classification == vg::hal::LoweringClass::CachedObject)
      return true;
  return false;
}

bool run_cpu_fixture() {
  // CPU-only setup/semantic check; it intentionally does not stand in for
  // the Vulkan GPU CTest.
  vg::core::Arena arena;
  auto &holder = arena.allocate(16);
  auto &target = arena.allocate(4);
  uint8_t wire[12]{};
  std::memcpy(wire, &target.id, 8);
  std::memcpy(wire + 8, &target.generation, 4);
  if (!arena.copy_into(&holder, 0, wire, sizeof(wire)))
    return false;
  const auto module = pointer_module(holder, target);
  const auto package =
      vg::compiler::build_pointer_graph_compute_package(module);
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  auto reference = vg::reference::make_device_hal();
  vg::hal::CompiledPlan compiled;
  vg::hal::Submission submission;
  const bool passed =
      vg::ir::verify(module).ok && package.ok &&
      package.package.bindings.size() == 1 &&
      package.package.vulkan_glsl_source.find("void main()") !=
          std::string::npos &&
      vg::test_support::assemble_single_node_plan(
          arena, module,
          {vg::test_support::compute_task(holder.id, holder.generation)},
          &fixture, &plan, &error) &&
      reference->compile(plan, &compiled, &error) &&
      reference->submit(compiled, arena, &submission, &error) &&
      submission.result.ok && target.bytes == std::vector<uint8_t>(4, 0x2a);
  std::cerr << "pointer-graph: CPU-only Reference fixture "
            << (passed ? "passed" : "failed")
            << (error.empty() ? "" : ": " + error) << "\n";
  return passed;
}

bool run() {
  std::string error;
  const auto fail = [&](const char *stage) {
    std::cerr << "pointer-graph: " << stage
              << (error.empty() ? "" : ": " + error) << "\n";
    return false;
  };
  auto vk = vg::vulkan::make_device_hal(&error);
  if (!vk) {
    std::cerr << "pointer-graph: Vulkan device required: " << error << "\n";
    return false;
  }
  auto ref = vg::reference::make_device_hal();
  vg::core::Arena va, ra;
  auto &vh = va.allocate(16);
  auto &vt = va.allocate(4);
  auto &rh = ra.allocate(16);
  auto &rt = ra.allocate(4);
  const auto put_ref = [](vg::core::Arena &a, vg::core::Allocation &h,
                          const vg::core::Allocation &t) {
    uint8_t wire[12]{};
    std::memcpy(wire, &t.id, 8);
    std::memcpy(wire + 8, &t.generation, 4);
    return a.copy_into(&h, 0, wire, sizeof(wire));
  };
  if (!put_ref(va, vh, vt) || !put_ref(ra, rh, rt))
    return fail("could not initialize pointer holder bytes");
  const auto holder_before = vh.bytes;
  vg::test_support::AssembledPlanFixture vf, rf;
  vg::core::ExecutionPlan vp, rp;
  if (!vg::test_support::assemble_single_node_plan(
          va, pointer_module(vh, vt),
          {vg::test_support::compute_task(vh.id, vh.generation)}, &vf, &vp,
          &error) ||
      !vg::test_support::assemble_single_node_plan(
          ra, pointer_module(rh, rt),
          {vg::test_support::compute_task(rh.id, rh.generation)}, &rf, &rp,
          &error)) {
    std::cerr << "pointer-graph: assembly failed: " << error << "\n";
    return false;
  }
  vg::hal::CompiledPlan vc, rc;
  if (!vk->compile(vp, &vc, &error) || !cached(vc) ||
      !ref->compile(rp, &rc, &error)) {
    std::cerr << "pointer-graph: compile/classification failed: " << error
              << "\n";
    return false;
  }
  // A second Stage-6 compile must reuse the immutable package/pipeline
  // identity.
  vg::hal::CompiledPlan cached_vc;
  if (!vk->compile(vp, &cached_vc, &error) || !cached(cached_vc))
    return fail("second compile did not select a cached package");
  bool pipeline_reused = false;
  for (const auto &e : cached_vc.report.events)
    pipeline_reused =
        pipeline_reused ||
        (e.operation == "vulkan_pipeline" &&
         e.classification == vg::hal::LoweringClass::CachedObject &&
         e.reason.find("reused") != std::string::npos);
  if (!pipeline_reused ||
      vc.per_node_packages.front().ref.index !=
          cached_vc.per_node_packages.front().ref.index ||
      vc.per_node_packages.front().ref.generation !=
          cached_vc.per_node_packages.front().ref.generation ||
      vc.per_node_packages.front().package->canonical_ir_hash !=
          cached_vc.per_node_packages.front().package->canonical_ir_hash)
    return fail("second compile did not reuse its Vulkan pipeline/package identity");
  vg::hal::Submission vs, rs;
  if (!vk->submit(cached_vc, va, &vs, &error) || !vs.result.ok ||
      !ref->submit(rc, ra, &rs, &error) || !rs.result.ok ||
      vt.bytes != rt.bytes || vt.bytes.size() != 4 || vt.bytes[0] != 0x2a ||
      vt.bytes[1] != 0x2a || vt.bytes[2] != 0x2a || vt.bytes[3] != 0x2a ||
      vh.bytes != holder_before) {
    std::cerr << "pointer-graph: Vulkan/Reference observable store mismatch: "
              << error << "\n";
    return false;
  }
  // Stage 7 must reject an immutable package whose content was changed, before
  // effects.
  const auto expected_bytes = vt.bytes;
  const uint8_t sentinel[4] = {0x91, 0x92, 0x93, 0x94};
  if (!va.copy_into(&vt, 0, sentinel, sizeof(sentinel)))
    return fail("could not prepare tamper sentinel");
  const auto before = vt.bytes;
  auto tampered = cached_vc;
  tampered.per_node_packages.front().package->canonical_ir_hash += "-tampered";
  vg::hal::Submission rejected;
  error.clear();
  if (vk->submit(tampered, va, &rejected, &error) || vt.bytes != before ||
      error.find("compiled per-Node package disagrees with the resolved immutable module") ==
          std::string::npos ||
      vh.in_flight != 0 || vt.in_flight != 0)
    return fail("tampered package was not rejected before effects");
  // A repeated real submission retains the static package and produces the same
  // bytes.
  if (!vk->submit(cached_vc, va, &vs, &error) || !vs.result.ok ||
      vt.bytes != expected_bytes)
    return fail("repeated submission did not reproduce pointer store");
  if (vh.in_flight != 0 || vt.in_flight != 0) {
    std::cerr
        << "pointer-graph: submit did not release allocation lifetime holds\n";
    return false;
  }
  // Allocation creation changes Core's sealed topology epoch. Stage 7 must
  // refuse before any previously compiled static package can take effect.
  if (!va.copy_into(&vt, 0, sentinel, sizeof(sentinel)))
    return fail("could not prepare topology sentinel");
  const auto stale_before = vt.bytes;
  auto &unrelated = va.allocate(4);
  (void)unrelated;
  error.clear();
  if (vk->submit(cached_vc, va, &rejected, &error) ||
      error.find("graph epoch") == std::string::npos ||
      vt.bytes != stale_before || vh.in_flight != 0 || vt.in_flight != 0) {
    std::cerr
        << "pointer-graph: changed topology was not rejected before effects\n";
    return false;
  }
  // Declared-edge and package-width/alignment negatives belong to
  // verifier/package boundaries.
  auto bad_edge = pointer_module(vh, vt);
  bad_edge.declared_pointer_edges.front().to_allocation = vh.id;
  if (vg::ir::verify(bad_edge).ok)
    return fail("invalid declared edge passed IR verification");
  auto bad_size = pointer_module(vh, vt);
  bad_size.instructions[0].size = 8;
  if (vg::compiler::build_pointer_graph_compute_package(bad_size).ok)
    return fail("invalid pointer package width passed validation");
  auto bad_alignment = pointer_module(vh, vt);
  bad_alignment.instructions[1].offset = 2;
  if (vg::compiler::build_pointer_graph_compute_package(bad_alignment).ok)
    return fail("invalid pointer package alignment passed validation");

  // Separate Nodes prove that a full NodeRef, rather than a first/global
  // module projection, selects a linear package beside a pointer package.
  vg::core::Arena ma, mra;
  auto &mh = ma.allocate(16);
  auto &mp = ma.allocate(4);
  auto &ml = ma.allocate(4);
  auto &mrh = mra.allocate(16);
  auto &mrp = mra.allocate(4);
  auto &mrl = mra.allocate(4);
  if (!put_ref(ma, mh, mp) || !put_ref(mra, mrh, mrp))
    return false;
  vg::test_support::MultiNodePlanFixture mf, mrf;
  vg::core::ExecutionPlan multi, ref_multi;
  auto pt = vg::test_support::compute_task(mh.id, mh.generation);
  auto lt = vg::test_support::compute_task(ml.id, ml.generation);
  auto rpt = vg::test_support::compute_task(mrh.id, mrh.generation);
  auto rlt = vg::test_support::compute_task(mrl.id, mrl.generation);
  if (!vg::test_support::assemble_multi_node_plan(
          ma, {pointer_module(mh, mp), linear_store_module(ml)}, {pt, lt},
          {{0, 1}}, &mf, &multi, &error) ||
      !vg::test_support::assemble_multi_node_plan(
          mra, {pointer_module(mrh, mrp), linear_store_module(mrl)}, {rpt, rlt},
          {{0, 1}}, &mrf, &ref_multi, &error))
    return false;
  vg::hal::CompiledPlan mc, mrc;
  vg::hal::Submission ms, mrs;
  if (!vk->compile(multi, &mc, &error) || mc.per_node_packages.size() != 2 ||
      !cached(mc) || !ref->compile(ref_multi, &mrc, &error) ||
      !vk->submit(mc, ma, &ms, &error) || !ms.result.ok ||
      !ref->submit(mrc, mra, &mrs, &error) || !mrs.result.ok ||
      mp.bytes != mrp.bytes || ml.bytes != mrl.bytes || mp.bytes[0] != 42 ||
      ml.bytes[0] != 7 || mf.node_refs[0].generation == 0 ||
      mf.node_refs[1].generation == 0 || mh.in_flight != 0 ||
      mp.in_flight != 0 || ml.in_flight != 0)
    return false;
  return true;
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--cpu-fixture")
    return run_cpu_fixture() ? 0 : 1;
  if (argc != 2) {
    std::cerr
        << "usage: vg_vulkan_pointer_graph_test <repo_root>|--cpu-fixture\n";
    return 2;
  }
  return run() ? 0 : 1;
}
