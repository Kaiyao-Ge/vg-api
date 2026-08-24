#include "backends/device_hal.h"
#include "backends/reference/reference_device_hal.h"
#include "backends/reference/reference_executor.h"
#include "compiler/compiler.h"
#include "ir/ir.h"

#include <cassert>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

struct TaskChain {
  uint32_t task_count{};
  uint64_t root_allocation{};
};

vg::core::TaskGraph make_chain(TaskChain chain) {
  vg::core::TaskGraphBuilder builder;
  for (uint32_t i = 0; i < chain.task_count; ++i) {
    vg::core::TaskRecord task{};
    task.node_index = i;
    task.root_allocation = chain.root_allocation;
    assert(builder.append(task));
    if (i > 0) assert(builder.add_dependency(i - 1, i));
  }
  vg::core::TaskGraph graph;
  assert(builder.seal(&graph));
  assert(graph.publish());
  return graph;
}

vg::ir::Module make_module(vg::core::Arena& arena) {
  auto compiled = vg::compiler::compile_c_like("@node @effects store(1,0,4,7)");
  assert(compiled.ok);
  auto& root = arena.allocate(64);
  compiled.module.instructions[0].allocation = root.id;
  compiled.module.declared_effects[0].allocation = root.id;
  compiled.module.canonical_json = vg::ir::serialize_module(compiled.module);
  return compiled.module;
}

struct Fixture {
  vg::core::Arena arena;
  std::unique_ptr<vg::hal::DeviceHal> device;
  vg::hal::CompiledPlan compiled;
  vg::core::TaskGraph oracle_graph;

  explicit Fixture(uint32_t task_count) {
    device = vg::reference::make_device_hal();
    assert(device != nullptr);
    const auto module = make_module(arena);
    const uint64_t root = module.instructions[0].allocation;
    oracle_graph = make_chain({.task_count = task_count, .root_allocation = root});
    vg::hal::ExecutionPlan plan;
    plan.capabilities = device->capabilities();
    plan.module = module;
    plan.published = true;
    plan.task_graph = oracle_graph;
    plan.graph_epoch = arena.topology_epoch();
    std::string error;
    assert(device->compile(plan, &compiled, &error));
  }

  vg::hal::CompiledPlan with_quota(uint32_t quota) const {
    auto copy = compiled;
    copy.plan.envelope_task_quota = quota;
    return copy;
  }
};

bool same_task(const vg::core::TaskRecord& a, const vg::core::TaskRecord& b) {
  return a.node_index == b.node_index && a.node_generation == b.node_generation &&
         a.root_allocation == b.root_allocation && a.root_generation == b.root_generation;
}

bool has_host_assisted(const vg::hal::Submission& submission) {
  for (const auto& event : submission.report.events) {
    if (event.operation == "envelope_continuation" &&
        event.classification == vg::hal::LoweringClass::HostAssisted)
      return true;
  }
  return false;
}

bool mentions_ring_overflow(const std::string& text) {
  return text.find("publication ring quota overflow") != std::string::npos;
}

}  // namespace

int main() {
  // Large quota: one submit, no overflow.
  {
    Fixture fixture(3);
    auto plan = fixture.with_quota(8);
    vg::hal::Submission submission;
    std::string error;
    assert(fixture.device->submit(plan, fixture.arena, &submission, &error));
    assert(submission.result.ok);
    assert(!submission.envelope_overflow.has_value());
    const auto oracle = vg::reference::execute_task_graph(fixture.oracle_graph);
    assert(oracle.ok);
    assert(submission.published_tasks.size() == oracle.published_tasks.size());
    for (size_t i = 0; i < oracle.published_tasks.size(); ++i)
      assert(same_task(submission.published_tasks[i], oracle.published_tasks[i]));
    assert(!fixture.device->envelope_continuations().contains(1));
  }

  // Unset quota is the pre-D5 no-op.
  {
    Fixture fixture(3);
    vg::hal::Submission submission;
    std::string error;
    assert(fixture.device->submit(fixture.compiled, fixture.arena, &submission, &error));
    assert(submission.result.ok);
    assert(!submission.envelope_overflow.has_value());
    assert(submission.published_tasks.size() == 3);
  }

  // Small quota + 3 tasks: first Deferred count 2; second with token finishes
  // with the leftover only; union equals the oracle and has no duplicates.
  {
    Fixture fixture(3);
    auto first_plan = fixture.with_quota(1);
    vg::hal::Submission first;
    std::string error;
    assert(fixture.device->submit(first_plan, fixture.arena, &first, &error));
    assert(first.result.ok);
    assert(first.published_tasks.size() == 1);
    assert(first.published_tasks[0].node_index == 0);
    assert(first.envelope_overflow.has_value());
    assert(first.envelope_overflow->valid());
    assert(first.envelope_overflow->disposition == vg::core::EnvelopeOverflowDisposition::Deferred);
    assert(first.envelope_overflow->overflow_task_count == 2);
    assert(first.envelope_overflow->continuation_token != 0);
    assert(first.envelope_overflow->continued());
    assert(has_host_assisted(first));
    assert(!mentions_ring_overflow(first.report.canonical_json()));
    assert(fixture.device->envelope_continuations().contains(first.envelope_overflow->continuation_token));

    auto continued = first_plan;
    continued.plan.pending_overflow = first.envelope_overflow;
    vg::hal::Submission second;
    assert(fixture.device->submit(continued, fixture.arena, &second, &error));
    assert(second.result.ok);
    assert(second.published_tasks.size() == 2);
    assert(second.published_tasks[0].node_index == 1);
    assert(second.published_tasks[1].node_index == 2);
    assert(!second.envelope_overflow.has_value());
    assert(!fixture.device->envelope_continuations().contains(first.envelope_overflow->continuation_token));

    std::vector<vg::core::TaskRecord> published = first.published_tasks;
    published.insert(published.end(), second.published_tasks.begin(), second.published_tasks.end());
    const auto oracle = vg::reference::execute_task_graph(fixture.oracle_graph);
    assert(oracle.ok);
    assert(published.size() == oracle.published_tasks.size());
    std::set<uint32_t> seen;
    for (size_t i = 0; i < published.size(); ++i) {
      assert(same_task(published[i], oracle.published_tasks[i]));
      assert(seen.insert(published[i].node_index).second);
    }
  }

  // Second submit without the token must not steal leftover.
  {
    Fixture fixture(3);
    auto first_plan = fixture.with_quota(1);
    vg::hal::Submission first;
    std::string error;
    assert(fixture.device->submit(first_plan, fixture.arena, &first, &error));
    const uint64_t token = first.envelope_overflow->continuation_token;
    assert(fixture.device->envelope_continuations().contains(token));

    vg::hal::Submission stolen;
    assert(fixture.device->submit(first_plan, fixture.arena, &stolen, &error));
    assert(stolen.result.ok);
    assert(stolen.published_tasks.size() == 1);
    assert(stolen.published_tasks[0].node_index == 0);
    assert(stolen.envelope_overflow.has_value());
    assert(stolen.envelope_overflow->continuation_token != token);
    assert(fixture.device->envelope_continuations().contains(token));

    auto continued = first_plan;
    continued.plan.pending_overflow = first.envelope_overflow;
    vg::hal::Submission drain;
    assert(fixture.device->submit(continued, fixture.arena, &drain, &error));
    assert(drain.result.ok);
    assert(drain.published_tasks.size() == 2);
    assert(drain.published_tasks[0].node_index == 1);
    assert(!fixture.device->envelope_continuations().contains(token));
  }

  // A larger quota on the continuation submit still publishes leftover only.
  {
    Fixture fixture(3);
    auto first_plan = fixture.with_quota(1);
    vg::hal::Submission first;
    std::string error;
    assert(fixture.device->submit(first_plan, fixture.arena, &first, &error));
    auto continued = first_plan;
    continued.plan.envelope_task_quota = 100;
    continued.plan.pending_overflow = first.envelope_overflow;
    vg::hal::Submission second;
    assert(fixture.device->submit(continued, fixture.arena, &second, &error));
    assert(second.published_tasks.size() == 2);
    assert(second.published_tasks[0].node_index == 1);
    assert(second.published_tasks[1].node_index == 2);
  }

  // Rejected pending: refuse, never continued().
  {
    Fixture fixture(3);
    auto plan = fixture.with_quota(1);
    vg::core::EnvelopeOverflow rejected;
    rejected.disposition = vg::core::EnvelopeOverflowDisposition::Rejected;
    rejected.overflow_task_count = 2;
    assert(rejected.valid());
    assert(!rejected.continued());
    plan.plan.pending_overflow = rejected;
    vg::hal::Submission submission;
    std::string error;
    assert(!fixture.device->submit(plan, fixture.arena, &submission, &error));
    assert(error == "envelope leftover was rejected");
    assert(!mentions_ring_overflow(error));
    assert(!rejected.continued());
    assert(submission.published_tasks.empty());
  }

  // Bad token: refuse, leftover stays.
  {
    Fixture fixture(3);
    auto first_plan = fixture.with_quota(1);
    vg::hal::Submission first;
    std::string error;
    assert(fixture.device->submit(first_plan, fixture.arena, &first, &error));
    const uint64_t token = first.envelope_overflow->continuation_token;
    auto continued = first_plan;
    continued.plan.pending_overflow = first.envelope_overflow;
    continued.plan.pending_overflow->continuation_token = token + 99;
    vg::hal::Submission bad;
    assert(!fixture.device->submit(continued, fixture.arena, &bad, &error));
    assert(error == "envelope continuation token does not match");
    assert(!mentions_ring_overflow(error));
    assert(fixture.device->envelope_continuations().contains(token));
    assert(bad.published_tasks.empty());
  }

  return 0;
}
