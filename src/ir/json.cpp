#include "ir/json.h"
#include <cctype>
#include <stdexcept>

namespace vg::json {
bool Value::is_object() const { return std::holds_alternative<Object>(data); }
bool Value::is_array() const { return std::holds_alternative<Array>(data); }
bool Value::is_string() const { return std::holds_alternative<std::string>(data); }
bool Value::is_int() const { return std::holds_alternative<int64_t>(data); }
const Value::Object& Value::object() const { return std::get<Object>(data); }
const Value::Array& Value::array() const { return std::get<Array>(data); }
const std::string& Value::string() const { return std::get<std::string>(data); }
int64_t Value::integer() const { return std::get<int64_t>(data); }
const Value* Value::find(const std::string& key) const { if (!is_object()) return nullptr; auto it=object().find(key); return it==object().end()?nullptr:&it->second; }

namespace {
class Parser {
 public: explicit Parser(const std::string& input) : input_(input) {}
  Value run() { Value value=value_at(); space(); if (pos_ != input_.size()) fail("trailing input"); return value; }
 private:
  void space() { while(pos_<input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_; }
  [[noreturn]] void fail(const char* message) const { throw std::runtime_error(std::string("JSON parse error: ")+message); }
  char take() { if(pos_==input_.size()) fail("unexpected end"); return input_[pos_++]; }
  Value value_at() {
    space(); if(pos_==input_.size()) fail("missing value"); char c=input_[pos_];
    if(c=='{') return object_at(); if(c=='[') return array_at(); if(c=='\"') return Value(string_at());
    if(c=='-' || std::isdigit(static_cast<unsigned char>(c))) return number_at();
    if(input_.compare(pos_,4,"null")==0) {pos_+=4; return Value();}
    if(input_.compare(pos_,4,"true")==0) {pos_+=4; Value v; v.data=true; return v;}
    if(input_.compare(pos_,5,"false")==0) {pos_+=5; Value v; v.data=false; return v;}
    fail("unsupported token");
  }
  Value object_at() { take(); Value::Object result; space(); if(pos_<input_.size()&&input_[pos_]=='}'){++pos_;return Value(std::move(result));}
    while(true){ space(); if(take()!='\"') fail("object key"); std::string key=string_body(); space(); if(take()!=':') fail("object colon"); result.emplace(std::move(key),value_at()); space(); char c=take(); if(c=='}') break; if(c!=',') fail("object separator"); } return Value(std::move(result)); }
  Value array_at() { take(); Value::Array result; space(); if(pos_<input_.size()&&input_[pos_]==']'){++pos_;return Value(std::move(result));}
    while(true){ result.push_back(value_at()); space(); char c=take(); if(c==']') break; if(c!=',') fail("array separator"); } return Value(std::move(result)); }
  std::string string_at() { if(take()!='\"') fail("string"); return string_body(); }
  std::string string_body() { std::string result; while(true){ char c=take(); if(c=='\"') return result; if(c=='\\'){ char e=take(); switch(e){case '\"':case '\\':case '/':result+=e;break;case 'b':result+='\b';break;case 'f':result+='\f';break;case 'n':result+='\n';break;case 'r':result+='\r';break;case 't':result+='\t';break;default:fail("unsupported escape");}} else { if(static_cast<unsigned char>(c)<0x20) fail("control character"); result+=c; } } }
  Value number_at() { size_t begin=pos_; if(input_[pos_]=='-')++pos_; if(pos_==input_.size()||!std::isdigit(static_cast<unsigned char>(input_[pos_])))fail("number"); while(pos_<input_.size()&&std::isdigit(static_cast<unsigned char>(input_[pos_])))++pos_; if(pos_<input_.size()&&(input_[pos_]=='.'||input_[pos_]=='e'||input_[pos_]=='E')) fail("floating point is not canonical IR"); try{return Value(std::stoll(input_.substr(begin,pos_-begin)));}catch(...){fail("integer range");} }
  const std::string& input_; size_t pos_{};
};
void append_escaped(const std::string& input, std::string& out) { out+='\"'; for(unsigned char c:input){switch(c){case '\"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default: if(c<0x20) throw std::runtime_error("unsupported control character"); out+=static_cast<char>(c);}} out+='\"'; }
void write(const Value& value,std::string& out){ if(std::holds_alternative<std::nullptr_t>(value.data)){out+="null";}else if(auto b=std::get_if<bool>(&value.data)){out+=*b?"true":"false";}else if(auto i=std::get_if<int64_t>(&value.data)){out+=std::to_string(*i);}else if(auto s=std::get_if<std::string>(&value.data)){append_escaped(*s,out);}else if(auto a=std::get_if<Value::Array>(&value.data)){out+='[';for(size_t i=0;i<a->size();++i){if(i)out+=',';write((*a)[i],out);}out+=']';}else{out+='{';bool first=true;for(const auto& [key,child]:std::get<Value::Object>(value.data)){if(!first)out+=',';first=false;append_escaped(key,out);out+=':';write(child,out);}out+='}';}}
}
Value parse(const std::string& text) { return Parser(text).run(); }
std::string canonical(const Value& value) { std::string out; write(value,out); return out; }
}
