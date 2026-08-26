#ifndef VG_IR_IR_H_
#define VG_IR_IR_H_
#include "ir/json.h"
#include <cstdint>
#include <string>
#include <vector>

namespace vg::ir {
enum class Access : uint64_t { None = 0, Read = 1, Write = 2, Atomic = 4, Publish = 8 };
Access access_from_op(const std::string& op, Access unknown = Access::Read);
struct Effect { uint64_t allocation{}; uint64_t offset{}; uint64_t size{}; Access access{}; uint32_t representation_epoch{}; };
// ref_operand is only meaningful for load_via/store_via (E002): a 1-based
// index into Module::instructions naming the load_ref this instruction
// dereferences through. 0 means unused (every other op).
struct Instruction { std::string op; uint64_t allocation{}; uint64_t offset{}; uint64_t size{}; int64_t value{}; uint32_t generation{}; uint32_t representation_epoch{}; uint64_t ref_operand{}; std::string source; };
// A single declared hop of a typed pointer graph (E002): "a load_ref whose
// ref value lives at (from_allocation, field_offset) is allowed to be
// dereferenced by a load_via/store_via naming to_allocation". Multi-hop
// chains are expressed as a sequence of load_ref instructions, each keyed
// off the previous hop's target -- this struct only encodes one hop, so
// verify() only ever needs a direct-edge lookup, never a graph walk.
struct PointerEdge { uint64_t from_allocation{}; uint64_t field_offset{}; uint64_t to_allocation{}; };
struct Module { uint32_t version{1}; std::string root_schema; std::vector<Instruction> instructions; std::vector<Effect> declared_effects; std::vector<PointerEdge> declared_pointer_edges; std::string canonical_json; std::string hash; };
struct VerifyResult { bool ok{}; std::string message; std::vector<Effect> inferred_effects; };
Module parse_module(const std::string& text);
std::string serialize_module(const Module& module);
VerifyResult verify(const Module& module);
bool effect_covers(const Effect& declared, const Effect& actual);
// F3 (ADR-043 Decision #4): the declared effect contract for a restricted-
// import hand-written MSL raster shader -- root_schema here is an unrelated
// concept from Module::root_schema (same field name, different meaning: this
// one names the shader's declared root schema, not an IR module's) and must
// never be cross-assigned with it. The compiler validates only this
// caller-declared shape, never the MSL source's logic (source is carried
// through untouched). vertex_abi is a required, exact declaration of the
// vertex-buffer bytes consumed by both the fixed Metal binding and the F4
// producer contract; it prevents an F3 xyuv producer from being silently
// interpreted as F4 xyzuv-packed data.
inline constexpr const char kRasterVertexAbiXyzuvPackedV1[] = "vg.raster.vertex.xyzuv-packed/v1";
struct UserRasterShaderContract {
  std::string root_schema;
  std::string vertex_entry;
  std::string fragment_entry;
  std::string vertex_abi;
  std::string source;
};
UserRasterShaderContract parse_msl_raster_envelope(const std::string& text);
}
#endif
