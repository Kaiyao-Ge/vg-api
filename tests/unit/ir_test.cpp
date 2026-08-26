#include "compiler/compiler.h"
#include "ir/ir.h"
#include "ir/sha256.h"
#include <cassert>
#include <stdexcept>
#include <string>

// F3 (ADR-043 Decision #4): parse_msl_raster_envelope(text) throws on any of
// the 4 required fields being absent or empty -- see it call require()
// itself for "IR missing field: X" (json key altogether absent) versus its
// own post-extraction empty-string check for "MSL raster envelope missing
// field: X" (key present, value is ""). Both are exercised below since the
// implementation distinguishes them with different messages.
void expect_msl_envelope_throw(const std::string& json_text, const std::string& expected_substring) {
  bool threw = false;
  try {
    vg::ir::parse_msl_raster_envelope(json_text);
  } catch (const std::exception& e) {
    threw = true;
    assert(std::string(e.what()).find(expected_substring) != std::string::npos);
  }
  assert(threw);
}

int main() {
  // NIST FIPS 180-2 SHA-256("abc"). PipelineKey::hash() and module.hash both
  // go through ir::sha256_hex; if this vector fails, E013's cache and IR
  // identity are measuring something that is not SHA-256.
  assert(vg::ir::sha256_hex("abc") ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string source = "@node @effects load(1,0,8) store(1,8,8,7)";
  auto compiled = vg::compiler::compile_c_like(source);
  assert(compiled.ok);
  assert(compiled.module.instructions.size() == 2);
  auto parsed = vg::ir::parse_module(compiled.module.canonical_json);
  assert(parsed.hash == compiled.module.hash);
  assert(vg::ir::verify(parsed).ok);
  auto invalid_module = parsed;
  invalid_module.instructions[0].generation = 0;
  assert(!vg::ir::verify(invalid_module).ok);
  invalid_module = parsed;
  invalid_module.instructions[0].offset = UINT64_MAX;
  invalid_module.instructions[0].size = 2;
  assert(!vg::ir::verify(invalid_module).ok);
  invalid_module = parsed;
  invalid_module.declared_effects.clear();
  assert(!vg::ir::verify(invalid_module).ok);
  auto invalid = vg::compiler::compile_c_like("@node @effects for (;;) {}");
  assert(!invalid.ok);

  // F3 (ADR-043 Decision #4): ir::parse_msl_raster_envelope round-trips the
  // 4-field envelope a "vg.msl.raster/v1" CodeObject carries -- root_schema
  // here is the shader's own declared root schema, an unrelated concept from
  // Module::root_schema above despite the shared field name (ir.h's
  // UserRasterShaderContract comment).
  {
    const std::string envelope_json =
        R"({"root_schema":"vg.test.raster/v1","vertex_entry":"vg_test_vertex",)"
        R"("fragment_entry":"vg_test_fragment","source":"#include <metal_stdlib>\nusing namespace metal;\n"})";
    const auto contract = vg::ir::parse_msl_raster_envelope(envelope_json);
    assert(contract.root_schema == "vg.test.raster/v1");
    assert(contract.vertex_entry == "vg_test_vertex");
    assert(contract.fragment_entry == "vg_test_fragment");
    assert(contract.source == "#include <metal_stdlib>\nusing namespace metal;\n");
  }

  // One rejection per field: the key entirely absent from the JSON object.
  expect_msl_envelope_throw(
      R"({"vertex_entry":"v","fragment_entry":"f","source":"s"})", "IR missing field: root_schema");
  expect_msl_envelope_throw(
      R"({"root_schema":"r","fragment_entry":"f","source":"s"})", "IR missing field: vertex_entry");
  expect_msl_envelope_throw(
      R"({"root_schema":"r","vertex_entry":"v","source":"s"})", "IR missing field: fragment_entry");
  expect_msl_envelope_throw(
      R"({"root_schema":"r","vertex_entry":"v","fragment_entry":"f"})", "IR missing field: source");

  // ...and separately, the key present but an empty string -- the
  // implementation distinguishes this case with its own message.
  expect_msl_envelope_throw(
      R"({"root_schema":"","vertex_entry":"v","fragment_entry":"f","source":"s"})",
      "MSL raster envelope missing field: root_schema");
  expect_msl_envelope_throw(
      R"({"root_schema":"r","vertex_entry":"","fragment_entry":"f","source":"s"})",
      "MSL raster envelope missing field: vertex_entry");
  expect_msl_envelope_throw(
      R"({"root_schema":"r","vertex_entry":"v","fragment_entry":"","source":"s"})",
      "MSL raster envelope missing field: fragment_entry");
  expect_msl_envelope_throw(
      R"({"root_schema":"r","vertex_entry":"v","fragment_entry":"f","source":""})",
      "MSL raster envelope missing field: source");

  return 0;
}
