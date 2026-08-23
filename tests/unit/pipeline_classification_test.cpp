// Unit coverage for the E013 side of Phase C: 07 §9's four StateBlock fates,
// 06 §7's pipeline cache key fields, and 05 §11's specialization report.
// Assert-based like tests/unit/core_test.cpp -- no test framework, so a
// failure is a plain abort at the line that states the requirement.
#include "compiler/pipeline_classification.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

vg::compiler::PipelineKey base_key() {
  vg::compiler::PipelineKey key;
  key.code_object_hash = "b0f1c2d3e4a5";
  key.entry = "vg_raster_fragment";
  key.attachment_formats = {70, 80};
  key.sample_count = 1;
  key.target_identity = "macos-15.1/apple-m1/metal-3.2";
  return key;
}

vg::compiler::StateBlock block(std::string name, vg::compiler::StateBlockKind kind, uint64_t value) {
  vg::compiler::StateBlock out;
  out.name = std::move(name);
  out.kind = kind;
  out.value = value;
  return out;
}

}  // namespace

int main() {
  const vg::compiler::PipelineKey base = base_key();

  // --- classify_pipeline_state: 07 §9's taxonomy is what decides whether a
  // piece of state may multiply pipelines. A PipelineKey block is compiled in
  // (it becomes a raster_state entry); DynamicState and ShaderVisibleData are
  // recorded but must never reach key.canonical(), which is the structural
  // half of E013's "可 shader-data/dynamic 的状态不会进入 pipeline key". ---
  {
    const std::vector<vg::compiler::StateBlock> blocks{
        block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, 2),
        block("viewport", vg::compiler::StateBlockKind::DynamicState, 4),
        block("scissor", vg::compiler::StateBlockKind::DynamicState, 5),
        block("material_tint", vg::compiler::StateBlockKind::ShaderVisibleData, 0xdeadbeefu),
    };
    const auto classified = vg::compiler::classify_pipeline_state(base, blocks);
    assert(classified.ok);
    assert(classified.unsupported.empty());
    assert(classified.key.raster_state.size() == 1);
    assert(classified.key.raster_state[0].first == "cull_mode");
    assert(classified.key.raster_state[0].second == 2);
    assert(classified.dynamic_state.size() == 2);
    assert(classified.shader_visible_data.size() == 1);

    const std::string canonical = classified.key.canonical();
    assert(canonical.find("cull_mode") != std::string::npos);
    assert(canonical.find("viewport") == std::string::npos);
    assert(canonical.find("scissor") == std::string::npos);
    assert(canonical.find("material_tint") == std::string::npos);
    // The excluded sets are reported rather than merely absent: 10 §12's
    // no-hidden-cost reconciliation needs "this state stayed out of the key"
    // to be a checkable statement.
    assert(classified.message.find("Node entry '" + base.entry + "'") != std::string::npos);
    assert(classified.message.find("stay out of the key") != std::string::npos);

    // The key the classification produced is the *only* thing that changed:
    // every structural field the caller supplied on `base` survives untouched.
    assert(classified.key.code_object_hash == base.code_object_hash);
    assert(classified.key.entry == base.entry);
    assert(classified.key.attachment_formats == base.attachment_formats);
    assert(classified.key.sample_count == base.sample_count);
    assert(classified.key.target_identity == base.target_identity);

    // Two classifications differing only on their dynamic/shader-visible
    // blocks reach the identical key -- the E013 claim, stated as an equality.
    const std::vector<vg::compiler::StateBlock> other_dynamic{
        block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, 2),
        block("viewport", vg::compiler::StateBlockKind::DynamicState, 999),
        block("scissor", vg::compiler::StateBlockKind::DynamicState, 111),
        block("material_tint", vg::compiler::StateBlockKind::ShaderVisibleData, 7),
    };
    const auto same_key = vg::compiler::classify_pipeline_state(base, other_dynamic);
    assert(same_key.ok);
    assert(same_key.key.canonical() == classified.key.canonical());
    assert(same_key.key.hash() == classified.key.hash());
  }

  // --- UnsupportedNeedsConversion is reported, never folded into a key
  // (START.md invariant 10, 06 §6.2). The diagnostic speaks VG concepts and
  // names the offending block, per 05 §14. ---
  {
    const std::vector<vg::compiler::StateBlock> blocks{
        block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, 2),
        block("logic_op_xor", vg::compiler::StateBlockKind::UnsupportedNeedsConversion, 3),
    };
    const auto rejected = vg::compiler::classify_pipeline_state(base, blocks);
    assert(!rejected.ok);
    assert(rejected.unsupported.size() == 1);
    assert(rejected.unsupported[0].name == "logic_op_xor");
    assert(rejected.message.find("'logic_op_xor'") != std::string::npos);
    assert(rejected.message.find("Node entry '" + base.entry + "'") != std::string::npos);
    assert(rejected.message.find("was not folded into the pipeline key") != std::string::npos);
    // The rejected block really is absent from the key it would have polluted.
    assert(rejected.key.canonical().find("logic_op_xor") == std::string::npos);
  }

  // --- Two PipelineKey blocks sharing a name but disagreeing on value would
  // make the resulting key depend on caller iteration order, so it is a
  // failure rather than a silent last-writer-wins. Same name *and* same value
  // is idempotent, not a conflict. ---
  {
    const std::vector<vg::compiler::StateBlock> conflicting{
        block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, 1),
        block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, 2),
    };
    const auto conflict = vg::compiler::classify_pipeline_state(base, conflicting);
    assert(!conflict.ok);
    assert(conflict.message.find("'cull_mode'") != std::string::npos);
    assert(conflict.message.find("classification order") != std::string::npos);

    const std::vector<vg::compiler::StateBlock> repeated{
        block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, 1),
        block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, 1),
    };
    const auto deduplicated = vg::compiler::classify_pipeline_state(base, repeated);
    assert(deduplicated.ok);
    assert(deduplicated.key.raster_state.size() == 1);
  }

  // --- PipelineKey::canonical(): deterministic, order-insensitive for the two
  // named-pair fields it sorts, order-*sensitive* for attachment formats, and
  // length-delimited so no two distinct keys can produce the same text by
  // concatenation accident (06 §7). hash() is the first 8 bytes of
  // ir::sha256_hex(canonical()); after the message-schedule pack was corrected
  // to write four bytes per word, two distinct canonical texts produce two
  // distinct digests, which is the contract E013's cache hit/miss count
  // depends on (format and sample_count live late in the canonical text). ---
  {
    vg::compiler::PipelineKey unsorted = base;
    unsorted.function_constants = {{"tile_size", 32}, {"cascade_count", 4}};
    unsorted.raster_state = {{"cull_mode", 2}, {"blend_enable", 0}};
    vg::compiler::PipelineKey sorted = base;
    sorted.function_constants = {{"cascade_count", 4}, {"tile_size", 32}};
    sorted.raster_state = {{"blend_enable", 0}, {"cull_mode", 2}};
    assert(unsorted.canonical() == sorted.canonical());
    // Equal canonical text must mean equal digest -- the direction of the
    // hash contract that a cache hit depends on.
    assert(unsorted.hash() == sorted.hash());
    // Deterministic across calls, not merely equal to a sibling.
    assert(unsorted.canonical() == unsorted.canonical());
    assert(unsorted.hash() == unsorted.hash());

    // attachment index is meaning, not noise.
    vg::compiler::PipelineKey swapped_attachments = base;
    swapped_attachments.attachment_formats = {80, 70};
    assert(swapped_attachments.canonical() != base.canonical());

    // sample_count participates in the key.
    vg::compiler::PipelineKey msaa = base;
    msaa.sample_count = 4;
    assert(msaa.canonical() != base.canonical());

    // A value differing only in which field a shared character sequence lands
    // in stays distinguishable, which is what the length delimiters buy.
    vg::compiler::PipelineKey left;
    left.code_object_hash = "ab";
    left.entry = "c";
    vg::compiler::PipelineKey right;
    right.code_object_hash = "a";
    right.entry = "bc";
    assert(left.canonical() != right.canonical());

    // target_identity is never interpreted, only hashed -- but it is in the
    // key, because 05 §10 makes the backend binary cache non-portable.
    vg::compiler::PipelineKey other_target = base;
    other_target.target_identity = "macos-15.2/apple-m1/metal-3.2";
    assert(other_target.canonical() != base.canonical());
    // Distinct canonical texts must produce distinct digests: otherwise the
    // cache would treat two incompatible targets (or two attachment orders,
    // or two sample counts) as the same pipeline (06 §7, E013).
    assert(other_target.hash() != base.hash());
    assert(swapped_attachments.hash() != base.hash());
    assert(msaa.hash() != base.hash());
  }

  // --- PipelineClassificationCache: 05 §11 requires a specialization to
  // report compile time, cache key, cache hit, binary size, trigger reason and
  // fallback. A hit compiles nothing (compile_ns == 0, `create` not called);
  // a failed create counts the attempted miss but records no pipeline. ---
  {
    vg::compiler::PipelineClassificationCache cache;
    std::string cache_error;
    uint32_t create_calls = 0;
    const auto create = [&create_calls](uint64_t* binary_size, std::string*) {
      ++create_calls;
      *binary_size = 4096;
      return true;
    };

    vg::compiler::SpecializationReport miss;
    assert(cache.acquire(base, "first draw of this material", create, &miss, &cache_error));
    assert(!miss.cache_hit);
    assert(miss.cache_key == base.hash());
    assert(miss.binary_size == 4096);
    assert(miss.trigger_reason == "first draw of this material");
    // 05 §11's fallback is filled by whoever actually substitutes a baseline;
    // this layer never invents one it did not perform.
    assert(miss.fallback.empty());
    assert(create_calls == 1);
    assert(cache.pipeline_count() == 1);
    assert(cache.cache_misses() == 1);
    assert(cache.cache_hits() == 0);

    vg::compiler::SpecializationReport hit;
    assert(cache.acquire(base, "second draw of this material", create, &hit, &cache_error));
    assert(hit.cache_hit);
    assert(hit.compile_ns == 0);
    assert(hit.cache_key == miss.cache_key);
    assert(hit.binary_size == 4096);
    assert(create_calls == 1);
    assert(cache.pipeline_count() == 1);
    assert(cache.cache_hits() == 1);
    assert(cache.cache_misses() == 1);
    assert(cache.reports().size() == 2);

    // A failed create leaves no entry and no report: the miss really was
    // attempted, but nothing is recorded as a pipeline that exists.
    vg::compiler::PipelineKey failing = base;
    failing.entry = "vg_raster_fragment_broken";
    const auto failing_create = [](uint64_t*, std::string* error) {
      *error = "Node entry 'vg_raster_fragment_broken' failed MSL compilation";
      return false;
    };
    vg::compiler::SpecializationReport failed;
    assert(!cache.acquire(failing, "broken specialization", failing_create, &failed, &cache_error));
    assert(cache_error == "Node entry 'vg_raster_fragment_broken' failed MSL compilation");
    assert(cache.pipeline_count() == 1);
    assert(cache.cache_misses() == 2);
    assert(cache.reports().size() == 2);

    // Retrying that key is still a miss -- the failure recorded nothing that
    // a later acquire could be served from.
    vg::compiler::SpecializationReport retried;
    assert(cache.acquire(failing, "broken specialization retried", create, &retried, &cache_error));
    assert(!retried.cache_hit);
    assert(cache.pipeline_count() == 2);
    assert(cache.cache_misses() == 3);
    assert(cache.cache_hits() == 1);
    assert(create_calls == 2);

    // A miss with no callback to compile it is an honest rejection, not a
    // fabricated entry. It is refused before the miss is counted, because no
    // compile was attempted.
    vg::compiler::PipelineKey uncompilable = base;
    uncompilable.entry = "vg_absent";
    vg::compiler::SpecializationReport none;
    assert(!cache.acquire(uncompilable, "no callback", nullptr, &none, &cache_error));
    assert(cache_error.find("no specialization callback") != std::string::npos);
    assert(cache.pipeline_count() == 2);
    assert(cache.cache_misses() == 3);

    // A missing report output is refused rather than dropped on the floor.
    assert(!cache.acquire(base, "no report", create, nullptr, &cache_error));
    assert(cache_error == "specialization report output is required");

    cache.clear();
    assert(cache.pipeline_count() == 0);
    assert(cache.cache_hits() == 0);
    assert(cache.cache_misses() == 0);
    assert(cache.reports().empty());
  }

  // --- Distinct attachment orders are distinct keys (06 §7: attachment index
  // is meaning). After the SHA-256 message schedule packs four bytes per word,
  // they no longer share a digest, so the second acquire is a real miss and
  // a second pipeline -- not a collision refusal. ---
  {
    vg::compiler::PipelineClassificationCache cache;
    std::string cache_error;
    const auto create = [](uint64_t* binary_size, std::string*) {
      *binary_size = 2048;
      return true;
    };

    vg::compiler::PipelineKey first = base;
    first.attachment_formats = {70, 80};
    vg::compiler::PipelineKey swapped = base;
    swapped.attachment_formats = {80, 70};
    assert(first.canonical() != swapped.canonical());
    assert(first.hash() != swapped.hash());

    vg::compiler::SpecializationReport report;
    assert(cache.acquire(first, "first attachment order", create, &report, &cache_error));
    assert(!report.cache_hit);

    vg::compiler::SpecializationReport second;
    assert(cache.acquire(swapped, "swapped attachment order", create, &second, &cache_error));
    assert(!second.cache_hit);
    assert(cache.cache_hits() == 0);
    assert(cache.cache_misses() == 2);
    assert(cache.pipeline_count() == 2);
    assert(cache.reports().size() == 2);
  }

  // --- E013's arithmetic oracle. Three binary axes -- one PipelineKey block,
  // one DynamicState block, one ShaderVisibleData block -- give 8 acquires,
  // but only the key axis is allowed to multiply pipelines. So the expected
  // numbers are exactly 2 pipelines, 2 misses, 6 hits, 2 compiles: "VG 不增加
  // 必须固定的组合" as a count rather than as a claim. ---
  {
    // The classification-level statement first, which is the part that depends
    // on nothing but classify_pipeline_state: 8 permutations collapse to 2
    // distinct canonical key texts.
    std::vector<std::string> distinct_canonicals;
    for (uint64_t key_axis = 0; key_axis < 2; ++key_axis) {
      for (uint64_t dynamic_axis = 0; dynamic_axis < 2; ++dynamic_axis) {
        for (uint64_t data_axis = 0; data_axis < 2; ++data_axis) {
          const std::vector<vg::compiler::StateBlock> blocks{
              block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, key_axis),
              block("viewport", vg::compiler::StateBlockKind::DynamicState, dynamic_axis),
              block("material_tint", vg::compiler::StateBlockKind::ShaderVisibleData, data_axis),
          };
          const auto classified = vg::compiler::classify_pipeline_state(base, blocks);
          assert(classified.ok);
          const std::string canonical = classified.key.canonical();
          bool seen = false;
          for (const auto& known : distinct_canonicals) seen = seen || known == canonical;
          if (!seen) distinct_canonicals.push_back(canonical);
        }
      }
    }
    assert(distinct_canonicals.size() == 2);
  }
  {
    // ...then the cache-level counting. The two key-axis values are the
    // natural 0 and 1: a correct SHA-256 digest distinguishes them, so the
    // 8 acquires collapse to 2 pipelines / 2 misses / 6 hits without having
    // to stretch the values to dodge a broken mixer.
    vg::compiler::PipelineKey first_axis = base;
    first_axis.raster_state = {{"cull_mode", 0}};
    vg::compiler::PipelineKey second_axis = base;
    second_axis.raster_state = {{"cull_mode", 1}};
    assert(first_axis.hash() != second_axis.hash());

    vg::compiler::PipelineClassificationCache cache;
    std::string cache_error;
    uint32_t compiles = 0;
    const auto create = [&compiles](uint64_t* binary_size, std::string*) {
      ++compiles;
      *binary_size = 1024;
      return true;
    };

    const uint64_t key_axis_values[2] = {0, 1};
    for (uint64_t key_axis = 0; key_axis < 2; ++key_axis) {
      for (uint64_t dynamic_axis = 0; dynamic_axis < 2; ++dynamic_axis) {
        for (uint64_t data_axis = 0; data_axis < 2; ++data_axis) {
          const std::vector<vg::compiler::StateBlock> blocks{
              block("cull_mode", vg::compiler::StateBlockKind::PipelineKey, key_axis_values[key_axis]),
              block("viewport", vg::compiler::StateBlockKind::DynamicState, dynamic_axis),
              block("material_tint", vg::compiler::StateBlockKind::ShaderVisibleData, data_axis),
          };
          const auto classified = vg::compiler::classify_pipeline_state(base, blocks);
          assert(classified.ok);
          vg::compiler::SpecializationReport report;
          assert(cache.acquire(classified.key, "E013 permutation", create, &report, &cache_error));
        }
      }
    }
    assert(cache.pipeline_count() == 2);
    assert(cache.cache_misses() == 2);
    assert(cache.cache_hits() == 6);
    assert(compiles == 2);
    assert(cache.reports().size() == 8);
    // Only the two compiles report a compile time; the six hits compiled
    // nothing, which is the whole point of the classification (05 §11).
    uint32_t timed_reports = 0;
    for (const auto& report : cache.reports()) {
      if (report.cache_hit) {
        assert(report.compile_ns == 0);
      } else {
        ++timed_reports;
      }
    }
    assert(timed_reports == 2);
  }

  return 0;
}
