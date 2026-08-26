#include "ir/ir.h"
#include "ir/sha256.h"
#include <algorithm>
#include <stdexcept>

namespace vg::ir {
namespace {
const json::Value& require(const json::Value::Object& object, const std::string& key) { auto it=object.find(key); if(it==object.end()) throw std::runtime_error("IR missing field: "+key); return it->second; }
uint64_t u64(const json::Value& value, const char* field) { if(!value.is_int()||value.integer()<0) throw std::runtime_error(std::string("IR invalid unsigned field: ")+field); return static_cast<uint64_t>(value.integer()); }
uint32_t u32(const json::Value& value, const char* field) { const uint64_t result=u64(value,field); if(result>UINT32_MAX) throw std::runtime_error(std::string("IR field exceeds u32: ")+field); return static_cast<uint32_t>(result); }
Access parse_access(const std::string& value) { if(value=="none")return Access::None; if(value=="read")return Access::Read; if(value=="write")return Access::Write; if(value=="atomic")return Access::Atomic; if(value=="publish")return Access::Publish; throw std::runtime_error("IR invalid access: "+value); }
std::string access_name(Access access) { switch(access){case Access::None:return "none";case Access::Read:return "read";case Access::Write:return "write";case Access::Atomic:return "atomic";case Access::Publish:return "publish";} return "unknown"; }
Effect parse_effect(const json::Value& value) { const auto& o=value.object(); return {u64(require(o,"allocation"),"allocation"),u64(require(o,"offset"),"offset"),u64(require(o,"size"),"size"),parse_access(require(o,"access").string()),u32(require(o,"representation_epoch"),"representation_epoch")}; }
json::Value effect_json(const Effect& effect) { return json::Value(json::Value::Object{{"access",json::Value(access_name(effect.access))},{"allocation",json::Value(static_cast<int64_t>(effect.allocation))},{"offset",json::Value(static_cast<int64_t>(effect.offset))},{"representation_epoch",json::Value(static_cast<int64_t>(effect.representation_epoch))},{"size",json::Value(static_cast<int64_t>(effect.size))}}); }
PointerEdge parse_pointer_edge(const json::Value& value) { const auto& o=value.object(); return {u64(require(o,"from_allocation"),"from_allocation"),u64(require(o,"field_offset"),"field_offset"),u64(require(o,"to_allocation"),"to_allocation")}; }
json::Value pointer_edge_json(const PointerEdge& edge) { return json::Value(json::Value::Object{{"field_offset",json::Value(static_cast<int64_t>(edge.field_offset))},{"from_allocation",json::Value(static_cast<int64_t>(edge.from_allocation))},{"to_allocation",json::Value(static_cast<int64_t>(edge.to_allocation))}}); }
bool is_pointer_via(const std::string& op) { return op=="load_via"||op=="store_via"; }
}
Access access_from_op(const std::string& op, Access unknown) {
  if (op == "load" || op == "load_ref" || op == "load_via") return Access::Read;
  if (op == "store" || op == "store_via") return Access::Write;
  if (op == "atomic_add") return Access::Atomic;
  if (op == "publish") return Access::Publish;
  return unknown;
}
Module parse_module(const std::string& text) {
  json::Value document=json::parse(text); if(!document.is_object()) throw std::runtime_error("IR root must be an object"); const auto& o=document.object();
  if(require(o,"schema").string()!="vg.ir/v1") throw std::runtime_error("unsupported IR schema");
  Module module; module.version=u32(require(o,"version"),"version"); module.root_schema=require(o,"root_schema").string();
  for(const auto& value:require(o,"instructions").array()){const auto& item=value.object(); Instruction i; i.op=require(item,"op").string(); i.allocation=u64(require(item,"allocation"),"allocation"); i.offset=u64(require(item,"offset"),"offset"); i.size=u64(require(item,"size"),"size"); i.generation=u32(require(item,"generation"),"generation"); if(const auto* p=value.find("representation_epoch"))i.representation_epoch=u32(*p,"representation_epoch"); if(const auto* p=value.find("value")){if(!p->is_int()) throw std::runtime_error("IR instruction value must be an integer"); i.value=p->integer();} if(const auto* p=value.find("ref_operand"))i.ref_operand=u64(*p,"ref_operand"); if(const auto* p=value.find("source")){if(!p->is_string()) throw std::runtime_error("IR instruction source must be a string"); i.source=p->string();} module.instructions.push_back(std::move(i));}
  if(const auto* effects=document.find("effects"))for(const auto& effect:effects->array())module.declared_effects.push_back(parse_effect(effect));
  if(const auto* edges=document.find("pointer_edges"))for(const auto& edge:edges->array())module.declared_pointer_edges.push_back(parse_pointer_edge(edge));
  module.canonical_json=serialize_module(module); module.hash=sha256_hex(module.canonical_json); return module;
}
std::string serialize_module(const Module& module) {
  json::Value::Array instructions; for(const auto& i:module.instructions){json::Value::Object o{{"allocation",json::Value(static_cast<int64_t>(i.allocation))},{"generation",json::Value(static_cast<int64_t>(i.generation))},{"offset",json::Value(static_cast<int64_t>(i.offset))},{"op",json::Value(i.op)},{"representation_epoch",json::Value(static_cast<int64_t>(i.representation_epoch))},{"size",json::Value(static_cast<int64_t>(i.size))}}; if(i.op=="store"||i.op=="atomic_add"||i.op=="store_via")o.emplace("value",json::Value(i.value)); if(is_pointer_via(i.op))o.emplace("ref_operand",json::Value(static_cast<int64_t>(i.ref_operand))); if(!i.source.empty())o.emplace("source",json::Value(i.source));instructions.emplace_back(std::move(o));}
  json::Value::Array effects;for(const auto& effect:module.declared_effects)effects.push_back(effect_json(effect));
  json::Value::Array pointer_edges;for(const auto& edge:module.declared_pointer_edges)pointer_edges.push_back(pointer_edge_json(edge));
  return json::canonical(json::Value(json::Value::Object{{"effects",json::Value(std::move(effects))},{"instructions",json::Value(std::move(instructions))},{"pointer_edges",json::Value(std::move(pointer_edges))},{"root_schema",json::Value(module.root_schema)},{"schema",json::Value("vg.ir/v1")},{"version",json::Value(static_cast<int64_t>(module.version))}}));
}
UserRasterShaderContract parse_msl_raster_envelope(const std::string& text) {
  json::Value document=json::parse(text); if(!document.is_object()) throw std::runtime_error("MSL raster envelope root must be an object"); const auto& o=document.object();
  UserRasterShaderContract contract;
  contract.root_schema=require(o,"root_schema").string(); if(contract.root_schema.empty()) throw std::runtime_error("MSL raster envelope missing field: root_schema");
  contract.vertex_entry=require(o,"vertex_entry").string(); if(contract.vertex_entry.empty()) throw std::runtime_error("MSL raster envelope missing field: vertex_entry");
  contract.fragment_entry=require(o,"fragment_entry").string(); if(contract.fragment_entry.empty()) throw std::runtime_error("MSL raster envelope missing field: fragment_entry");
  contract.vertex_abi=require(o,"vertex_abi").string(); if(contract.vertex_abi.empty()) throw std::runtime_error("MSL raster envelope missing field: vertex_abi");
  if(contract.vertex_abi != kRasterVertexAbiXyzuvPackedV1) throw std::runtime_error("MSL raster envelope has unsupported vertex_abi: "+contract.vertex_abi);
  contract.source=require(o,"source").string(); if(contract.source.empty()) throw std::runtime_error("MSL raster envelope missing field: source");
  return contract;
}
bool effect_covers(const Effect& declared,const Effect& actual){ if(declared.allocation!=actual.allocation||declared.representation_epoch!=actual.representation_epoch)return false; if((static_cast<uint64_t>(declared.access)&static_cast<uint64_t>(actual.access))!=static_cast<uint64_t>(actual.access))return false; if(actual.size==0||declared.size==0||declared.offset>actual.offset)return false; const uint64_t relative=actual.offset-declared.offset; return relative<=declared.size && actual.size<=declared.size-relative; }
namespace {
bool pointer_edge_covers(const Module& module,const Instruction& root,const Instruction& via){
  return std::ranges::any_of(module.declared_pointer_edges,[&](const PointerEdge& edge){ return edge.from_allocation==root.allocation && edge.field_offset==root.offset && edge.to_allocation==via.allocation; });
}
}
VerifyResult verify(const Module& module) {
  VerifyResult result{true,"",{}}; if(module.version!=1){result.ok=false;result.message="unsupported IR version";return result;} if(module.root_schema.empty()){result.ok=false;result.message="root schema is required";return result;} if(module.instructions.empty()){result.ok=false;result.message="IR module has no instructions";return result;}
  for(size_t index=0;index<module.instructions.size();++index){ const auto& i=module.instructions[index];
    if(i.allocation==0){result.ok=false;result.message="allocation identity must be non-zero";return result;} if(i.generation==0){result.ok=false;result.message="instruction generation must be non-zero";return result;} if(i.size==0){result.ok=false;result.message="zero-sized instruction";return result;} if(i.offset>UINT64_MAX-i.size){result.ok=false;result.message="instruction range overflows";return result;}
    Access access; if(i.op=="load")access=Access::Read;else if(i.op=="store")access=Access::Write;else if(i.op=="atomic_add")access=Access::Atomic;else if(i.op=="publish")access=Access::Publish;else if(i.op=="load_ref")access=Access::Read;else if(i.op=="load_via")access=Access::Read;else if(i.op=="store_via")access=Access::Write;else{result.ok=false;result.message="unsupported instruction: "+i.op;return result;}
    if(is_pointer_via(i.op)){
      if(i.ref_operand<1||i.ref_operand>index){result.ok=false;result.message="ref_operand must name a prior instruction";return result;}
      const Instruction& root=module.instructions[i.ref_operand-1];
      if(root.op!="load_ref"){result.ok=false;result.message="ref_operand must name a load_ref instruction";return result;}
      if(!pointer_edge_covers(module,root,i)){result.ok=false;result.message="declared pointer edges do not cover this dereference";return result;}
      continue;
    }
    result.inferred_effects.push_back({i.allocation,i.offset,i.size,access,i.representation_epoch});
  }
  for(const auto& actual:result.inferred_effects){bool covered=std::ranges::any_of(module.declared_effects,[&](const Effect& declared){return effect_covers(declared,actual);});if(!covered){result.ok=false;result.message="declared effects do not cover inferred access";return result;}}
  return result;
}
}
