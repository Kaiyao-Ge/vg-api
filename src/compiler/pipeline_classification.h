#ifndef VG_COMPILER_PIPELINE_CLASSIFICATION_H_
#define VG_COMPILER_PIPELINE_CLASSIFICATION_H_
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
namespace vg::compiler {

// E013's whole question -- "Node/StateBlock 分类是否减少无意义 pipeline
// permutation" -- only has a measurable answer if every piece of state is
// assigned one of exactly four fates before a key is built. 07 §9 names those
// four explicitly (shader-readable dynamic data, backend dynamic state,
// pipeline-key state, unsupported/transform-required state) and 06 §7 adds the
// budget constraint they exist to satisfy: "小的动态状态不应无故扩大 key".
// The enum is backend-neutral on purpose. Which fate a given piece of state
// receives is a backend judgement (Metal must compile in raster state that
// Vulkan can leave dynamic), but the taxonomy and the counting discipline are
// shared, so the two adapters' pipeline counts are comparable numbers rather
// than each backend's private accounting.
enum class StateBlockKind : uint32_t {
  PipelineKey,               // must be compiled in; enters the key
  DynamicState,              // set at encode time; must not enter the key
  ShaderVisibleData,         // plain data the shader reads; must not enter the key
  UnsupportedNeedsConversion // cannot be expressed; report, never fold into a key
};

// A single named piece of pipeline-adjacent state, already classified by
// whichever layer knows the backend's real constraints. `value` is a plain
// uint64 rather than a variant: everything 06 §7 lists as key-eligible
// (function-constant values, format codes, sample counts, enum-shaped raster
// state) is an integer or interns to one, and a wider payload would invite
// exactly the "fold anything into the key" habit E013 is measuring against.
struct StateBlock {
  std::string name;
  StateBlockKind kind{StateBlockKind::ShaderVisibleData};
  uint64_t value{};
};

// 06 §7's pipeline cache key fields, spelled out one per member so nothing
// enters the key by accident. Anything a caller wants keyed has to name a
// field here or arrive as a PipelineKey-classified StateBlock; there is no
// free-form bag for "everything else", which is the structural half of E013's
// "可 shader-data/dynamic 的状态不会进入 pipeline key" requirement.
// attachment_formats holds backend-neutral format codes and is deliberately
// order-sensitive (attachment index is meaning, not noise), unlike
// function_constants and raster_state which canonical() sorts by name.
// target_identity carries OS/GPU/compiler identity as one opaque string: it
// is never interpreted here, only hashed, because 05 §10 makes the backend
// binary cache explicitly non-portable and this key must not pretend
// otherwise.
struct PipelineKey {
  std::string code_object_hash;
  std::string entry;
  std::vector<std::pair<std::string, uint64_t>> function_constants;
  std::vector<uint32_t> attachment_formats;   // backend-neutral format codes
  uint32_t sample_count{1};
  std::vector<std::pair<std::string, uint64_t>> raster_state; // only what must be compiled in
  std::string target_identity;                // OS/GPU/compiler identity
  // Deterministic, sorted, stable text. Field-tagged and length-delimited so
  // two different keys cannot produce the same text by concatenation
  // accident, which would make the digest below silently alias them.
  std::string canonical() const;
  // First 8 bytes of sha256(canonical()), reused from ir/sha256.h rather than
  // an ad-hoc mixer so the digest is reproducible across runs, machines and
  // future readers of a recorded experiment artifact.
  uint64_t hash() const;
};

// The audit trail of one classification: not just the key, but what was kept
// out of it and why. 10 §12's "no hidden cost" reconciliation and E013's
// pipeline-count metric both need the excluded sets to be visible; a function
// that returned only the key would make "this state did not enter the key" an
// unverifiable claim.
struct PipelineClassification {
  PipelineKey key;
  std::vector<StateBlock> dynamic_state;        // deliberately excluded from key
  std::vector<StateBlock> shader_visible_data;  // deliberately excluded from key
  std::vector<StateBlock> unsupported;          // reported, not keyed
  bool ok{};
  std::string message;                          // 05 §14: speak VG concepts
};

// Folds `blocks` into `base`, honoring each block's classification: a
// PipelineKey block becomes a raster_state entry (the only key field that
// accepts free-form named state -- code object, entry, attachment formats and
// sample count are structural inputs the caller must supply on `base`), while
// DynamicState and ShaderVisibleData blocks are recorded in their own lists
// and never touch the key.
//
// Fails, with a message naming the offending block, when any block is
// UnsupportedNeedsConversion: START.md invariant 10 and 06 §6.2 both require
// an inexpressible state to surface as an explicit rejection instead of being
// folded into a key that would then compile a pipeline meaning something
// slightly different from what was asked for.
//
// Also fails on two PipelineKey blocks that share a name but disagree on
// value. Silently letting one win would make the resulting key depend on
// caller iteration order, which would break both the determinism canonical()
// promises and any cache-hit count measured on top of it.
PipelineClassification classify_pipeline_state(const PipelineKey& base,
                                               const std::vector<StateBlock>& blocks);

// 05 §11: "Specialization 必须报告：编译时间、cache key、cache hit、binary
// size、触发原因和 fallback." One struct with one field per required item, so
// a specialization that fails to report something is a visibly empty field
// rather than an omission nobody notices. `fallback` stays empty here and is
// filled by whoever actually substitutes a baseline -- this layer never
// invents a fallback it did not perform.
struct SpecializationReport {
  uint64_t cache_key{};
  bool cache_hit{};
  uint64_t compile_ns{};
  uint64_t binary_size{};
  std::string trigger_reason;
  std::string fallback;
};

// Backend-neutral pipeline cache keyed by PipelineKey::hash(). It lives in the
// compiler rather than in each backend so Metal, Vulkan and any future adapter
// share one hit/miss discipline: E013 compares pipeline counts across
// variants, and counts produced by separately invented bookkeeping are not
// comparable. The cache stores only measurements, never the backend pipeline
// object -- vg_compiler cannot name a MTLComputePipelineState or a
// VkPipeline, and the caller is the only party that can own its lifetime
// correctly anyway.
class PipelineClassificationCache {
 public:
  // Returns the slot for `key`, reporting through `out` whether it already
  // existed. `create` runs only on a miss and returns the backend object's
  // byte size (0 when the backend cannot measure it, which stays 0 in the
  // report rather than becoming a guess -- 10 §12 forbids writing an
  // unobservable cost as a real number). compile_ns is measured with
  // std::chrono::steady_clock around that call per 08 §8's monotonic
  // high-resolution clock requirement, and is 0 on a hit because a hit
  // compiles nothing.
  //
  // A failed `create` returns false and leaves no cache entry and no report:
  // the miss is counted (a compile really was attempted) but nothing is
  // recorded as a pipeline that exists. The caller decides and records the
  // 05 §11 fallback, because only the caller knows whether it had a declared
  // compatible baseline to fall back to.
  //
  // A hit whose stored canonical text disagrees with the incoming key is a
  // 64-bit digest collision; it is rejected rather than served, since serving
  // it would hand back a pipeline compiled for different state.
  bool acquire(const PipelineKey& key, const std::string& trigger_reason,
               const std::function<bool(uint64_t* binary_size, std::string* error)>& create,
               SpecializationReport* out, std::string* error = nullptr);
  uint32_t pipeline_count() const;
  uint32_t cache_hits() const;
  uint32_t cache_misses() const;
  const std::vector<SpecializationReport>& reports() const;
  void clear();

 private:
  struct Entry {
    std::string canonical;
    uint64_t binary_size{};
  };
  std::unordered_map<uint64_t, Entry> entries_;
  std::vector<SpecializationReport> reports_;
  uint32_t hits_{};
  uint32_t misses_{};
};

}  // namespace vg::compiler
#endif
