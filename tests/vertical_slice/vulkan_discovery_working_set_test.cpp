// Vulkan Stage 6/7 consumption probe.  Core assembles and seals discovery,
// certificates, leases and budgets; Vulkan may only consume/report them.
#include "../support/assembled_plan_fixture.h"
#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/vulkan/vulkan_device_hal.h"
#include "compiler/compiler.h"

#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

const vg::hal::LoweringEvent *event(const vg::hal::LoweringReport &report,
                                    const char *operation) {
  for (const auto &candidate : report.events)
    if (candidate.operation == operation)
      return &candidate;
  return nullptr;
}

void write_ref(vg::core::Allocation &allocation, vg::core::PointerRef ref) {
  std::memcpy(allocation.bytes.data(), &ref.allocation, sizeof(ref.allocation));
  std::memcpy(allocation.bytes.data() + sizeof(ref.allocation), &ref.generation,
              sizeof(ref.generation));
}

size_t active_count(const vg::core::Arena &arena) {
  size_t count = 0;
  for (const auto &[id, allocation] : arena.allocations()) {
    (void)id;
    if (allocation.state == vg::core::ObjectState::Active)
      ++count;
  }
  return count;
}

vg::ir::Module linear_store_module() {
  return vg::compiler::compile_c_like("@node @effects store(1,12,4,7)").module;
}

vg::ir::Module pointer_module(const vg::core::Allocation &holder,
                              const vg::core::Allocation &target) {
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/vulkan-discovery-pointer";
  module.instructions = {{"load_ref", holder.id, 0, 12, 0, holder.generation,
                          holder.representation_epoch, 0, ""},
                         {"store_via", target.id, 0, 4, 0x2a, target.generation,
                          target.representation_epoch, 1, ""}};
  module.declared_effects = {
      {holder.id, 0, 12, vg::ir::Access::Read, holder.representation_epoch}};
  module.declared_pointer_edges = {{holder.id, 0, target.id}};
  return module;
}

bool verify_store_and_oracle(const vg::core::Arena &vulkan,
                             const vg::core::Arena &reference,
                             vg::core::PointerRef root) {
  const auto *actual = vulkan.lookup(root);
  const auto *expected = reference.lookup(root);
  if (!actual || !expected || actual->bytes != expected->bytes ||
      actual->bytes.size() < 16 || actual->bytes[12] != 7 ||
      actual->bytes[13] != 7 || actual->bytes[14] != 7 ||
      actual->bytes[15] != 7) {
    std::cerr << "Vulkan output did not equal the Reference oracle's "
                 "observable store\n";
    return false;
  }
  return true;
}

bool assemble(
    vg::core::Arena &arena, const vg::ir::Module &module,
    vg::core::PointerRef root, vg::test_support::AssembledPlanFixture *fixture,
    vg::core::ExecutionPlan *plan, std::string *error,
    std::optional<vg::core::AccessCertificateMode> mode = std::nullopt,
    const std::vector<vg::core::PointerRef> *seeds = nullptr,
    const vg::core::WorkingSetBudget *budget = nullptr,
    const vg::core::WorkingSetLease *lease = nullptr) {
  vg::test_support::AssemblyOptions options;
  options.certificate_mode = mode;
  options.discovery_seeds = seeds;
  options.working_set_budget = budget;
  options.working_set_lease = lease;
  if (mode == vg::core::AccessCertificateMode::CertifiedPinned)
    options.certificate_touched = {root};
  return vg::test_support::assemble_single_node_plan(
      arena, module,
      {vg::test_support::compute_task(root.allocation, root.generation)},
      fixture, plan, error, options);
}

bool core_negative_assertions() {
  vg::core::Arena arena;
  auto &root = arena.allocate(16);
  auto &reached = arena.allocate(16);
  arena.allocate(16);
  write_ref(root, {reached.id, reached.generation});
  const auto module = linear_store_module();
  const vg::core::PointerRef root_ref{root.id, root.generation};
  const auto before = root.bytes;
  const auto no_hold = [&] {
    return arena.lookup(root_ref)->bytes == before &&
           arena.lookup(root_ref)->in_flight == 0;
  };
  vg::core::WorkingSetBudget small = vg::core::WorkingSetBudget::limited(16);
  vg::core::ExecutionPlan rejected;
  vg::test_support::AssembledPlanFixture fixture;
  std::string error;
  if (assemble(arena, module, root_ref, &fixture, &rejected, &error,
               vg::core::AccessCertificateMode::Universe, nullptr, &small) ||
      error != "working-set budget exceeded" || !no_hold())
    return false;
  vg::core::WorkingSetLease stale;
  stale.allocations.push_back({root.id, root.generation + 1});
  stale.complete = true;
  if (assemble(arena, module, root_ref, &fixture, &rejected, &error,
               vg::core::AccessCertificateMode::CertifiedPinned, nullptr,
               &small, &stale) ||
      error != "lease cannot cover an unproven allocation" || !no_hold())
    return false;
  const std::vector<vg::core::PointerRef> seeds{root_ref};
  vg::core::WorkingSetLease forged;
  forged.allocations.push_back({3, 1});
  forged.complete = true;
  if (assemble(arena, module, root_ref, &fixture, &rejected, &error,
               vg::core::AccessCertificateMode::DiscoverThenLease, &seeds,
               nullptr, &forged) ||
      error != "lease cannot cover an unproven allocation" || !no_hold())
    return false;
  return true;
}

bool submit_both(vg::hal::DeviceHal &vulkan,
                 const vg::core::ExecutionPlan &vulkan_plan,
                 vg::core::Arena &vulkan_arena,
                 const vg::core::ExecutionPlan &reference_plan,
                 vg::core::Arena &reference_arena,
                 vg::hal::Submission *vulkan_submission,
                 vg::hal::Submission *reference_submission,
                 std::string *error) {
  auto reference = vg::reference::make_device_hal();
  vg::hal::CompiledPlan vulkan_compiled, reference_compiled;
  if (!vulkan.compile(vulkan_plan, &vulkan_compiled, error) ||
      !vulkan_compiled.report.supported)
    return false;
  if (!reference ||
      !reference->compile(reference_plan, &reference_compiled, error))
    return false;
  if (!vulkan.submit(vulkan_compiled, vulkan_arena, vulkan_submission, error) ||
      !vulkan_submission->result.ok)
    return false;
  if (!reference->submit(reference_compiled, reference_arena,
                         reference_submission, error) ||
      !reference_submission->result.ok)
    return false;
  return true;
}

bool discovery_mode() {
  std::string device_error;
  auto vulkan = vg::vulkan::make_device_hal(&device_error);
  if (!vulkan) {
    std::cerr << "discovery: Vulkan device required: " << device_error << "\n";
    return false;
  }
  // Reachable n0->n1; n2/n3 are active but unreachable.  The operation itself
  // remains a canonical linear store, so this probe does not depend on B's
  // pointer lowering.
  vg::core::Arena va, ra;
  auto &vn0 = va.allocate(16);
  auto &vn1 = va.allocate(16);
  va.allocate(16);
  va.allocate(16);
  auto &rn0 = ra.allocate(16);
  auto &rn1 = ra.allocate(16);
  ra.allocate(16);
  ra.allocate(16);
  write_ref(vn0, {vn1.id, vn1.generation});
  write_ref(rn0, {rn1.id, rn1.generation});
  const std::vector<vg::core::PointerRef> vs{{vn0.id, vn0.generation}},
      rs{{rn0.id, rn0.generation}};
  const auto module = linear_store_module();
  vg::core::ExecutionPlan vp, rp;
  vg::test_support::AssembledPlanFixture vf, rf;
  std::string error;
  if (!assemble(va, module, vs.front(), &vf, &vp, &error,
                vg::core::AccessCertificateMode::DiscoverThenLease, &vs) ||
      !assemble(ra, module, rs.front(), &rf, &rp, &error,
                vg::core::AccessCertificateMode::DiscoverThenLease, &rs)) {
    std::cerr << "discovery assembly: " << error << "\n";
    return false;
  }
  const auto sealed = vp.access_certificate;
  if (!sealed || !vp.discovery_result ||
      sealed->epoch.references().size() != 2 ||
      sealed->epoch.references().size() >= active_count(va)) {
    std::cerr << "discovery core did not seal reachable subset\n";
    return false;
  }
  vg::hal::Submission vsub, rsub;
  if (!submit_both(*vulkan, vp, va, rp, ra, &vsub, &rsub, &error)) {
    std::cerr << "discovery submit: " << error << "\n";
    return false;
  }
  if (!verify_store_and_oracle(va, ra, vs.front()) ||
      va.lookup(vs.front())->in_flight != 0)
    return false;
  const auto *ve = event(vsub.report, "discovery");
  if (!vsub.access_certificate ||
      vsub.access_certificate->epoch.references().size() !=
          sealed->epoch.references().size() ||
      !ve || ve->classification != vg::hal::LoweringClass::HostAssisted ||
      event(vsub.report, "access_certificate")) {
    std::cerr << "discovery Vulkan did not consume the sealed HostAssisted "
                 "evidence\n";
    return false;
  }
  if (!rsub.access_certificate ||
      rsub.access_certificate->epoch.references().size() != 2)
    return false;
  // Core owns invalid/forged inputs before Vulkan can compile or submit.
  const auto before_forged = va.lookup(vs.front())->bytes;
  vg::core::WorkingSetLease forged;
  forged.allocations.push_back({3, 1});
  forged.complete = true;
  vg::core::ExecutionPlan rejected;
  vg::test_support::AssembledPlanFixture rejected_fixture;
  if (assemble(va, module, vs.front(), &rejected_fixture, &rejected, &error,
               vg::core::AccessCertificateMode::DiscoverThenLease, &vs, nullptr,
               &forged) ||
      error != "lease cannot cover an unproven allocation" ||
      va.lookup(vs.front())->bytes != before_forged ||
      va.lookup(vs.front())->in_flight != 0) {
    std::cerr << "discovery forged lease was accepted\n";
    return false;
  }
  // A topology mutation after Core freezes the certificate makes Stage 7 refuse
  // before dispatch.
  vg::core::Arena stale_arena;
  auto &stale_root = stale_arena.allocate(16);
  const std::vector<vg::core::PointerRef> stale_seed{
      {stale_root.id, stale_root.generation}};
  vg::core::ExecutionPlan stale_plan;
  vg::test_support::AssembledPlanFixture stale_fixture;
  if (!assemble(stale_arena, module, stale_seed.front(), &stale_fixture,
                &stale_plan, &error,
                vg::core::AccessCertificateMode::DiscoverThenLease,
                &stale_seed))
    return false;
  vg::hal::CompiledPlan stale_compiled;
  if (!vulkan->compile(stale_plan, &stale_compiled, &error))
    return false;
  const auto before = stale_arena.lookup(stale_seed.front())->bytes;
  stale_arena.allocate(16);
  vg::hal::Submission stale_submission;
  if (vulkan->submit(stale_compiled, stale_arena, &stale_submission, &error) ||
      error.find("graph epoch") == std::string::npos ||
      stale_arena.lookup(stale_seed.front())->bytes != before) {
    std::cerr
        << "discovery stale topology was not refused before side effects\n";
    return false;
  }
  // B+C integration: the restricted static-edge pointer package executes
  // under Core-sealed DiscoverThenLease evidence; this is separate from the
  // linear baseline above and has a Reference byte oracle.
  {
    vg::core::Arena pva, pra;
    auto &vh = pva.allocate(16);
    auto &vt = pva.allocate(4);
    auto &rh = pra.allocate(16);
    auto &rt = pra.allocate(4);
    write_ref(vh, {vt.id, vt.generation});
    write_ref(rh, {rt.id, rt.generation});
    const std::vector<vg::core::PointerRef> vseed{{vh.id, vh.generation}},
        rseed{{rh.id, rh.generation}};
    vg::core::ExecutionPlan pvp, prp;
    vg::test_support::AssembledPlanFixture pvf, prf;
    if (!assemble(pva, pointer_module(vh, vt), vseed.front(), &pvf, &pvp,
                  &error, vg::core::AccessCertificateMode::DiscoverThenLease,
                  &vseed) ||
        !assemble(pra, pointer_module(rh, rt), rseed.front(), &prf, &prp,
                  &error, vg::core::AccessCertificateMode::DiscoverThenLease,
                  &rseed))
      return false;
    vg::hal::Submission pvsub, prsub;
    if (!submit_both(*vulkan, pvp, pva, prp, pra, &pvsub, &prsub, &error) ||
        !event(pvsub.report, "discovery") ||
        event(pvsub.report, "discovery")->classification !=
            vg::hal::LoweringClass::HostAssisted ||
        !event(pvsub.report, "node_compute_package") ||
        event(pvsub.report, "node_compute_package")->classification !=
            vg::hal::LoweringClass::CachedObject ||
        vt.bytes != rt.bytes || vt.bytes != std::vector<uint8_t>(4, 0x2a) ||
        vh.in_flight != 0 || vt.in_flight != 0)
      return false;
  }
  std::cout << "discovery: reachable=2 universe=4 HostAssisted; forged lease "
               "rejected before Vulkan\n";
  return true;
}

bool working_set_mode() {
  std::string device_error;
  auto vulkan = vg::vulkan::make_device_hal(&device_error);
  if (!vulkan) {
    std::cerr << "working-set: Vulkan device required: " << device_error
              << "\n";
    return false;
  }
  const auto module = linear_store_module();
  if (!core_negative_assertions()) {
    std::cerr
        << "working-set core negative did not reject before side effects\n";
    return false;
  }
  // Exact budget and lease-only plans must execute on both backends and emit
  // Vulkan's proxy report.
  for (const uint64_t budget_limit : {uint64_t{64}, uint64_t{16}}) {
    vg::core::Arena va, ra;
    auto &vfroot = va.allocate(16);
    va.allocate(16);
    auto &rfroot = ra.allocate(16);
    ra.allocate(16);
    vg::core::WorkingSetBudget budget =
        vg::core::WorkingSetBudget::limited(budget_limit);
    vg::core::WorkingSetLease vl, rl;
    vl.allocations.push_back({vfroot.id, vfroot.generation});
    rl.allocations.push_back({rfroot.id, rfroot.generation});
    vl.byte_limit = rl.byte_limit = 16;
    vl.complete = rl.complete = true;
    vg::core::ExecutionPlan vp, rp;
    vg::test_support::AssembledPlanFixture vfixture, rfixture;
    std::string error;
    if (!assemble(va, module, {vfroot.id, vfroot.generation}, &vfixture, &vp,
                  &error, vg::core::AccessCertificateMode::CertifiedPinned,
                  nullptr, &budget, &vl) ||
        !assemble(ra, module, {rfroot.id, rfroot.generation}, &rfixture, &rp,
                  &error, vg::core::AccessCertificateMode::CertifiedPinned,
                  nullptr, &budget, &rl))
      return false;
    vg::hal::Submission vsub, rsub;
    if (!submit_both(*vulkan, vp, va, rp, ra, &vsub, &rsub, &error)) {
      std::cerr << "working-set submit: " << error << "\n";
      return false;
    }
    const auto *requested = event(vsub.report, "working_set_requested");
    const auto *committed = event(vsub.report, "working_set_committed");
    const auto *proxy = event(vsub.report, "working_set_proxy");
    const auto *sparse = event(vsub.report, "working_set_sparse");
    if (!requested || !committed || !proxy || !sparse ||
        requested->bytes != 16 || committed->bytes != 16 ||
        proxy->reason.find("proxy") == std::string::npos ||
        sparse->classification != vg::hal::LoweringClass::Unsupported)
      return false;
    if (!verify_store_and_oracle(va, ra, {vfroot.id, vfroot.generation}) ||
        va.lookup(vg::core::PointerRef{vfroot.id, vfroot.generation})
                ->in_flight != 0 ||
        vl.allocations.size() != 1 ||
        vl.allocations.front().allocation != vfroot.id)
      return false;
  }
  // No lease means Universe: its successful budget accounts for both Active
  // allocations.
  {
    vg::core::Arena va, ra;
    auto &vr = va.allocate(16);
    va.allocate(16);
    auto &rr = ra.allocate(16);
    ra.allocate(16);
    vg::core::WorkingSetBudget budget = vg::core::WorkingSetBudget::limited(32);
    vg::core::ExecutionPlan vp, rp;
    vg::test_support::AssembledPlanFixture vf, rf;
    std::string error;
    if (!assemble(va, module, {vr.id, vr.generation}, &vf, &vp, &error,
                  vg::core::AccessCertificateMode::Universe, nullptr,
                  &budget) ||
        !assemble(ra, module, {rr.id, rr.generation}, &rf, &rp, &error,
                  vg::core::AccessCertificateMode::Universe, nullptr, &budget))
      return false;
    vg::hal::Submission vsub, rsub;
    if (!submit_both(*vulkan, vp, va, rp, ra, &vsub, &rsub, &error) ||
        !event(vsub.report, "working_set_requested") ||
        event(vsub.report, "working_set_requested")->bytes != 32 ||
        !verify_store_and_oracle(va, ra, {vr.id, vr.generation}))
      return false;
  }
  // A lease without an ordinary budget remains a valid request and must not be
  // mistaken for sparse residency.
  {
    vg::core::Arena va, ra;
    auto &vr = va.allocate(16);
    auto &rr = ra.allocate(16);
    vg::core::WorkingSetLease vl, rl;
    vl.allocations.push_back({vr.id, vr.generation});
    rl.allocations.push_back({rr.id, rr.generation});
    vl.byte_limit = rl.byte_limit = 16;
    vl.complete = rl.complete = true;
    vg::core::ExecutionPlan vp, rp;
    vg::test_support::AssembledPlanFixture vf, rf;
    std::string error;
    if (!assemble(va, module, {vr.id, vr.generation}, &vf, &vp, &error,
                  vg::core::AccessCertificateMode::CertifiedPinned, nullptr,
                  nullptr, &vl) ||
        !assemble(ra, module, {rr.id, rr.generation}, &rf, &rp, &error,
                  vg::core::AccessCertificateMode::CertifiedPinned, nullptr,
                  nullptr, &rl))
      return false;
    vg::hal::Submission vsub, rsub;
    if (!submit_both(*vulkan, vp, va, rp, ra, &vsub, &rsub, &error) ||
        !event(vsub.report, "working_set_requested") ||
        event(vsub.report, "working_set_requested")->bytes != 16 ||
        !verify_store_and_oracle(va, ra, {vr.id, vr.generation}))
      return false;
  }
  // Paging modes are refused by Core before an adapter could manufacture a
  // fallback.
  vg::core::Arena arena;
  auto &root = arena.allocate(16);
  vg::core::ExecutionPlan rejected;
  vg::test_support::AssembledPlanFixture fixture;
  std::string error;
  for (const auto unsupported :
       {vg::core::AccessCertificateMode::SoftwarePaged,
        vg::core::AccessCertificateMode::FaultManaged}) {
    if (assemble(arena, module, {root.id, root.generation}, &fixture, &rejected,
                 &error, unsupported)) {
      std::cerr << "working-set: unsupported paging mode assembled\n";
      return false;
    }
  }
  std::cout << "working-set: exact/lease-only accepted; over-budget and stale "
               "lease rejected before Vulkan; sparse Unsupported\n";
  return true;
}

// This is deliberately a CPU-only fixture check for hosts where Vulkan is
// unsupported.  It is not a Vulkan CTest and never substitutes for one.
bool reference_fixture_mode() {
  auto reference = vg::reference::make_device_hal();
  if (!core_negative_assertions())
    return false;
  vg::core::Arena arena;
  auto &root = arena.allocate(16);
  auto &next = arena.allocate(16);
  write_ref(root, {next.id, next.generation});
  const std::vector<vg::core::PointerRef> seeds{{root.id, root.generation}};
  const auto module = linear_store_module();
  vg::core::ExecutionPlan plan;
  vg::test_support::AssembledPlanFixture fixture;
  std::string error;
  if (!reference ||
      !assemble(arena, module, seeds.front(), &fixture, &plan, &error,
                vg::core::AccessCertificateMode::DiscoverThenLease, &seeds))
    return false;
  vg::hal::CompiledPlan compiled;
  vg::hal::Submission submission;
  if (!reference->compile(plan, &compiled, &error) ||
      !reference->submit(compiled, arena, &submission, &error) ||
      !submission.result.ok || !submission.access_certificate ||
      submission.access_certificate->epoch.references().size() != 2 ||
      !event(submission.report, "discovery") ||
      arena.lookup(seeds.front())->bytes[12] != 7 ||
      arena.lookup(seeds.front())->in_flight != 0)
    return false;
  vg::core::Arena pointer_arena;
  auto &holder = pointer_arena.allocate(16);
  auto &target = pointer_arena.allocate(4);
  write_ref(holder, {target.id, target.generation});
  const std::vector<vg::core::PointerRef> pointer_seed{
      {holder.id, holder.generation}};
  vg::core::ExecutionPlan pointer_plan;
  vg::test_support::AssembledPlanFixture pointer_fixture;
  if (!assemble(pointer_arena, pointer_module(holder, target),
                pointer_seed.front(), &pointer_fixture, &pointer_plan, &error,
                vg::core::AccessCertificateMode::DiscoverThenLease,
                &pointer_seed) ||
      !reference->compile(pointer_plan, &compiled, &error) ||
      !reference->submit(compiled, pointer_arena, &submission, &error) ||
      !submission.result.ok || target.bytes != std::vector<uint8_t>(4, 0x2a) ||
      !event(submission.report, "discovery"))
    return false;
  std::cout << "reference-fixture-only: CPU semantic fixture passed; no Vulkan "
               "work executed\n";
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3 || (std::string(argv[1]) != "discovery" &&
                    std::string(argv[1]) != "working-set" &&
                    std::string(argv[1]) != "reference-fixture-only")) {
    std::cerr << "usage: vg_vulkan_discovery_working_set_test "
                 "discovery|working-set|reference-fixture-only <repo_root>\n";
    return 2;
  }
  if (std::string(argv[1]) == "discovery")
    return discovery_mode() ? 0 : 1;
  if (std::string(argv[1]) == "working-set")
    return working_set_mode() ? 0 : 1;
  return reference_fixture_mode() ? 0 : 1;
}
