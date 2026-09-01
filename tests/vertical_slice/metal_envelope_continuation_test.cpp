#include "backends/device_hal.h"
#include "backends/metal/metal_device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "compiler/compiler.h"
#include "../support/assembled_plan_fixture.h"

#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using vg::core::TaskGraph;
using vg::core::TaskRecord;

vg::ir::Module make_probe_module(vg::core::Arena& arena) {
  const auto& allocation = arena.allocate(64);
  vg::ir::Module module;
  module.version = 1;
  module.root_schema = "vg.test/v1";
  vg::ir::Instruction load;
  load.op = "load";
  load.allocation = allocation.id;
  load.generation = allocation.generation;
  load.representation_epoch = allocation.representation_epoch;
  load.offset = 0;
  load.size = 4;
  module.instructions.push_back(load);
  module.declared_effects.push_back(
      {allocation.id, 0, 64, vg::ir::Access::Read, allocation.representation_epoch});
  return module;
}

struct TaskChain {
  uint32_t task_count{};
  uint64_t root_allocation{};
};

bool assemble_chain(vg::core::Arena& arena, const vg::ir::Module& module, TaskChain chain,
                    vg::test_support::AssembledPlanFixture* fixture, vg::core::ExecutionPlan* plan,
                    std::string* error, uint32_t quota) {
  std::vector<TaskRecord> tasks;
  std::vector<std::pair<uint32_t, uint32_t>> dependencies;
  for (uint32_t i = 0; i < chain.task_count; ++i) {
    tasks.push_back(vg::test_support::compute_task(chain.root_allocation));
    if (i > 0) dependencies.emplace_back(i - 1, i);
  }
  vg::test_support::AssemblyOptions options;
  options.dependencies = &dependencies;
  options.task_quota = quota;
  return vg::test_support::assemble_single_node_plan(arena, module, tasks, fixture, plan, error, options);
}

bool same_task(const TaskRecord& a, const TaskRecord& b) {
  return a.node_index == b.node_index && a.node_generation == b.node_generation &&
         a.root_allocation == b.root_allocation && a.root_generation == b.root_generation && a.x == b.x &&
         a.y == b.y && a.z == b.z && a.flags == b.flags && a.contract_index == b.contract_index &&
         a.payload_size == b.payload_size && a.payload_or_offset == b.payload_or_offset;
}

bool run_reference_continuation() {
  auto device = vg::reference::make_device_hal();
  if (device == nullptr) {
    std::cerr << "envelope-continuation: no reference device\n";
    return false;
  }
  vg::core::Arena arena;
  const auto module = make_probe_module(arena);
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  if (!assemble_chain(arena, module, {.task_count = 3, .root_allocation = module.instructions[0].allocation},
                      &fixture, &plan, &error, 1)) {
    std::cerr << "envelope-continuation: failed to build task graph\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (!device->compile(plan, &compiled, &error)) {
    std::cerr << "envelope-continuation: reference compile failed: " << error << "\n";
    return false;
  }

  vg::hal::Submission first;
  if (!device->submit(compiled, arena, &first, &error) || !first.result.ok) {
    std::cerr << "envelope-continuation: first submit failed: " << error << " / "
              << first.result.message << "\n";
    return false;
  }
  if (!first.envelope_overflow.has_value() || !first.envelope_overflow->continued() ||
      first.envelope_overflow->overflow_task_count != 2 || first.published_tasks.size() != 1) {
    std::cerr << "envelope-continuation: first submit did not defer 2 leftover tasks\n";
    return false;
  }

  const uint64_t token = first.envelope_overflow->continuation_token;
  auto without_token = compiled;
  vg::hal::Submission stolen;
  if (!device->submit(without_token, arena, &stolen, &error) || !stolen.result.ok ||
      stolen.published_tasks.size() != 1 ||
      stolen.published_tasks[0].node_index != plan.task_graph.tasks()[0].node_index ||
      !device->envelope_continuations().contains(token)) {
    std::cerr << "envelope-continuation: submit without token must not steal leftover\n";
    return false;
  }

  auto continued = compiled;
  continued.plan.pending_overflow = first.envelope_overflow;
  vg::hal::Submission second;
  if (!device->submit(continued, arena, &second, &error) || !second.result.ok) {
    std::cerr << "envelope-continuation: continuation submit failed: " << error << "\n";
    return false;
  }
  if (second.published_tasks.size() != 2 || second.envelope_overflow.has_value() ||
      device->envelope_continuations().contains(token)) {
    std::cerr << "envelope-continuation: continuation did not drain leftover only\n";
    return false;
  }

  std::vector<TaskRecord> published = first.published_tasks;
  published.insert(published.end(), second.published_tasks.begin(), second.published_tasks.end());
  const auto oracle = vg::reference::execute_task_graph(plan.task_graph);
  if (!oracle.ok || published.size() != oracle.published_tasks.size()) {
    std::cerr << "envelope-continuation: published set size mismatches oracle\n";
    return false;
  }
  for (size_t i = 0; i < published.size(); ++i) {
    if (!same_task(published[i], oracle.published_tasks[i])) {
      std::cerr << "envelope-continuation: published task order mismatches oracle\n";
      return false;
    }
  }

  std::cout << "envelope-continuation: reference overflow buffer + next submit ok\n";
  return true;
}

bool run_metal_large_quota() {
  auto metal = vg::metal::make_device_hal();
  if (metal == nullptr) {
    std::cerr << "envelope-continuation: no Metal device available on this host\n";
    return false;
  }
  vg::core::Arena arena;
  const auto module = make_probe_module(arena);
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  if (!assemble_chain(arena, module, {.task_count = 3, .root_allocation = module.instructions[0].allocation},
                      &fixture, &plan, &error, 8)) {
    std::cerr << "envelope-continuation: failed to build Metal task graph\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (!metal->compile(plan, &compiled, &error)) {
    std::cerr << "envelope-continuation: Metal compile failed: " << error << "\n";
    return false;
  }
  vg::hal::Submission submission;
  if (!metal->submit(compiled, arena, &submission, &error) || !submission.result.ok) {
    std::cerr << "envelope-continuation: Metal large-quota submit failed: " << error << " / "
              << submission.result.message << "\n";
    return false;
  }
  const auto oracle = vg::reference::execute_task_graph(plan.task_graph);
  if (!oracle.ok || submission.published_tasks.size() != oracle.published_tasks.size()) {
    std::cerr << "envelope-continuation: Metal large-quota published_tasks count mismatch\n";
    return false;
  }
  for (size_t i = 0; i < oracle.published_tasks.size(); ++i) {
    if (!same_task(submission.published_tasks[i], oracle.published_tasks[i])) {
      std::cerr << "envelope-continuation: Metal published_tasks mismatch oracle\n";
      return false;
    }
  }
  if (submission.envelope_overflow.has_value()) {
    std::cerr << "envelope-continuation: Metal large quota must not record overflow\n";
    return false;
  }
  std::cout << "envelope-continuation: Metal large-quota one submit ok\n";
  return true;
}

bool run_metal_continuation() {
  auto metal = vg::metal::make_device_hal();
  if (metal == nullptr) {
    std::cerr << "envelope-continuation: no Metal device available on this host\n";
    return false;
  }
  vg::core::Arena arena;
  const auto module = make_probe_module(arena);
  vg::test_support::AssembledPlanFixture fixture;
  vg::core::ExecutionPlan plan;
  std::string error;
  if (!assemble_chain(arena, module, {.task_count = 3, .root_allocation = module.instructions[0].allocation},
                      &fixture, &plan, &error, 1)) {
    std::cerr << "envelope-continuation: failed to build Metal continuation graph\n";
    return false;
  }
  vg::hal::CompiledPlan compiled;
  if (!metal->compile(plan, &compiled, &error)) {
    std::cerr << "envelope-continuation: Metal continuation compile failed: " << error << "\n";
    return false;
  }

  vg::hal::Submission first;
  if (!metal->submit(compiled, arena, &first, &error) || !first.result.ok) {
    std::cerr << "envelope-continuation: Metal first submit failed: " << error << " / "
              << first.result.message << "\n";
    return false;
  }
  if (!first.envelope_overflow.has_value() || !first.envelope_overflow->continued() ||
      first.envelope_overflow->overflow_task_count != 2 || first.published_tasks.size() != 1) {
    std::cerr << "envelope-continuation: Metal first submit did not defer 2 leftover tasks\n";
    return false;
  }

  const uint64_t token = first.envelope_overflow->continuation_token;
  auto without_token = compiled;
  vg::hal::Submission stolen;
  if (!metal->submit(without_token, arena, &stolen, &error) || !stolen.result.ok ||
      stolen.published_tasks.size() != 1 ||
      stolen.published_tasks[0].node_index != plan.task_graph.tasks()[0].node_index ||
      !metal->envelope_continuations().contains(token)) {
    std::cerr << "envelope-continuation: Metal submit without token must not steal leftover\n";
    return false;
  }

  auto continued = compiled;
  continued.plan.pending_overflow = first.envelope_overflow;
  vg::hal::Submission second;
  if (!metal->submit(continued, arena, &second, &error) || !second.result.ok) {
    std::cerr << "envelope-continuation: Metal continuation submit failed: " << error << "\n";
    return false;
  }
  if (second.published_tasks.size() != 2 || second.envelope_overflow.has_value() ||
      metal->envelope_continuations().contains(token)) {
    std::cerr << "envelope-continuation: Metal continuation did not drain leftover only\n";
    return false;
  }

  std::vector<TaskRecord> published = first.published_tasks;
  published.insert(published.end(), second.published_tasks.begin(), second.published_tasks.end());
  const auto oracle = vg::reference::execute_task_graph(plan.task_graph);
  if (!oracle.ok || published.size() != oracle.published_tasks.size()) {
    std::cerr << "envelope-continuation: Metal published set size mismatches oracle\n";
    return false;
  }
  for (size_t i = 0; i < published.size(); ++i) {
    if (!same_task(published[i], oracle.published_tasks[i])) {
      std::cerr << "envelope-continuation: Metal published task order mismatches oracle\n";
      return false;
    }
  }

  std::cout << "envelope-continuation: Metal overflow buffer + next submit ok\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (!run_reference_continuation()) return 1;
  if (!run_metal_large_quota()) return 1;
  if (!run_metal_continuation()) return 1;
  return 0;
}
