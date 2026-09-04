#include "fixture.h"

namespace vg::tests::execution_plan {

using vg::hal::Capability;

vg::hal::CapabilitySnapshot fully_capable_reference_snapshot() {
  vg::hal::CapabilitySnapshot capabilities;
  capabilities.backend = vg::hal::BackendKind::Reference;
  capabilities.capability_bits = static_cast<uint64_t>(Capability::LinearAddress) |
                                  static_cast<uint64_t>(Capability::TaskPublication) |
                                  static_cast<uint64_t>(Capability::Timeline) |
                                  static_cast<uint64_t>(Capability::EffectDag) |
                                  static_cast<uint64_t>(Capability::CaptureReplay) |
                                  static_cast<uint64_t>(Capability::IndirectTier1) |
                                  static_cast<uint64_t>(Capability::Raster) |
                                  static_cast<uint64_t>(Capability::RepresentationTransform) |
                                  static_cast<uint64_t>(Capability::CheckedFacetGeneration) |
                                  static_cast<uint64_t>(Capability::UserShaderImport) |
                                  static_cast<uint64_t>(Capability::IndirectTier2Select) |
                                  static_cast<uint64_t>(Capability::IndexedBinding);
  capabilities.validation_available = true;
  return capabilities;
}

void expect_missing_capability(vg::core::ExecutionPlan plan, Capability missing,
                               const char* capability_name) {
  auto capabilities = fully_capable_reference_snapshot();
  capabilities.capability_bits &= ~static_cast<uint64_t>(missing);
  vg::hal::CompiledPlan compiled;
  std::string error;
  CHECK(!vg::hal::preflight_stage6(plan, capabilities, vg::hal::BackendKind::Reference,
                                   &compiled, &error));
  CHECK(error.find(capability_name) != std::string::npos);
  CHECK(!compiled.report.supported);
  CHECK(compiled.report.count(vg::hal::LoweringClass::Unsupported) >= 1);
}

vg::core::ExecutionPlan sealed_requirement_plan(std::initializer_list<vg::core::CapabilityRequirement> requirements) {
  vg::core::ExecutionPlan plan;
  plan.assembled = true;
  plan.capability_requirements_derived = true;
  plan.required_capabilities.assign(requirements);
  return plan;
}

void test_stage6_capability_preflight_rejects_without_weakening() {
  const auto basic = sealed_requirement_plan({vg::core::CapabilityRequirement::LinearAddress});
  expect_missing_capability(basic, Capability::LinearAddress, "LinearAddress");

  const auto indexed = sealed_requirement_plan({vg::core::CapabilityRequirement::IndexedBinding});
  expect_missing_capability(indexed, Capability::IndexedBinding, "IndexedBinding");

  const auto timeline = sealed_requirement_plan({vg::core::CapabilityRequirement::Timeline});
  expect_missing_capability(timeline, Capability::Timeline, "Timeline");

  const auto publication = sealed_requirement_plan({vg::core::CapabilityRequirement::TaskPublication});
  expect_missing_capability(publication, Capability::TaskPublication, "TaskPublication");

  const auto effect_dag = sealed_requirement_plan({vg::core::CapabilityRequirement::EffectDag});
  expect_missing_capability(effect_dag, Capability::EffectDag, "EffectDag");

  const auto representation = sealed_requirement_plan({vg::core::CapabilityRequirement::RepresentationTransform});
  expect_missing_capability(representation, Capability::RepresentationTransform,
                            "RepresentationTransform");

  const auto checked = sealed_requirement_plan({vg::core::CapabilityRequirement::CheckedFacetGeneration});
  expect_missing_capability(checked, Capability::CheckedFacetGeneration,
                            "CheckedFacetGeneration");

  const auto raster = sealed_requirement_plan({vg::core::CapabilityRequirement::Raster});
  expect_missing_capability(raster, Capability::Raster, "Raster");

  const auto user_shader = sealed_requirement_plan({vg::core::CapabilityRequirement::UserShaderImport});
  expect_missing_capability(user_shader, Capability::UserShaderImport, "UserShaderImport");

  const auto tier1 = sealed_requirement_plan({vg::core::CapabilityRequirement::IndirectTier1});
  expect_missing_capability(tier1, Capability::IndirectTier1, "IndirectTier1");

  const auto tier2 = sealed_requirement_plan({vg::core::CapabilityRequirement::IndirectTier2Select});
  expect_missing_capability(tier2, Capability::IndirectTier2Select, "IndirectTier2Select");

}

void test_validation_profile_matrix_and_reset() {
  auto plan = sealed_requirement_plan({});
  auto capabilities = fully_capable_reference_snapshot();
  vg::hal::CompiledPlan compiled;
  std::string error;

  for (const auto profile : {vg::core::ValidationProfile::FastNative,
                             vg::core::ValidationProfile::CheckedNative,
                             vg::core::ValidationProfile::ReferenceStrict,
                             vg::core::ValidationProfile::Capture}) {
    plan.required_capabilities.clear();
    if (profile == vg::core::ValidationProfile::CheckedNative)
      plan.required_capabilities = {vg::core::CapabilityRequirement::CheckedFacetGeneration};
    if (profile == vg::core::ValidationProfile::ReferenceStrict)
      plan.required_capabilities = {vg::core::CapabilityRequirement::ReferenceStrict};
    if (profile == vg::core::ValidationProfile::Capture)
      plan.required_capabilities = {vg::core::CapabilityRequirement::CaptureReplay};
    compiled.per_node_packages.emplace_back();
    compiled.representation_operations.push_back({});
    compiled.transition_operations.emplace_back();
    compiled.representation_operation_execution_order.push_back(0);
    CHECK(!vg::hal::preflight_stage6(plan, capabilities,
                                     vg::hal::BackendKind::Reference,
                                     &compiled, &error));
    CHECK(error.find("Stage6 requires a valid immutable core plan") !=
          std::string::npos);
    CHECK(compiled.per_node_packages.empty());
    CHECK(compiled.representation_operations.empty());
    CHECK(compiled.transition_operations.empty());
    CHECK(compiled.representation_operation_execution_order.empty());
  }

  plan.required_capabilities = {vg::core::CapabilityRequirement::ReferenceStrict};
  auto metal = capabilities;
  metal.backend = vg::hal::BackendKind::Metal;
  CHECK(!vg::hal::preflight_stage6(plan, metal, vg::hal::BackendKind::Metal, &compiled, &error));
  CHECK(error.find("ReferenceStrict") != std::string::npos);

  plan.required_capabilities = {vg::core::CapabilityRequirement::CheckedFacetGeneration};
  capabilities.validation_available = false;
  CHECK(!vg::hal::preflight_stage6(plan, capabilities, vg::hal::BackendKind::Reference,
                                   &compiled, &error));
  CHECK(error.find("CheckedNative") != std::string::npos);

  capabilities = fully_capable_reference_snapshot();
  plan.required_capabilities = {vg::core::CapabilityRequirement::CaptureReplay};
  capabilities.capability_bits &= ~static_cast<uint64_t>(Capability::CaptureReplay);
  compiled.per_node_packages.emplace_back();
  compiled.representation_operations.push_back({});
  compiled.transition_operations.emplace_back();
  compiled.representation_operation_execution_order.push_back(0);
  CHECK(!vg::hal::preflight_stage6(plan, capabilities, vg::hal::BackendKind::Reference,
                                   &compiled, &error));
  CHECK(compiled.per_node_packages.empty());
  CHECK(compiled.representation_operations.empty());
  CHECK(compiled.transition_operations.empty());
  CHECK(compiled.representation_operation_execution_order.empty());
}

void test_basic_execution_plan_validation() {
  vg::core::ExecutionPlan plan;
  std::string error;
  CHECK(!plan.validate(&error));
  CHECK(error == "execution plan must be produced by the canonical core assembler");
}

}  // namespace vg::tests::execution_plan
