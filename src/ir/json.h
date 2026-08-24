#ifndef VG_IR_JSON_H_
#define VG_IR_JSON_H_
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace vg::json {
struct Value {
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;
  std::variant<std::nullptr_t, bool, int64_t, std::string, Array, Object> data;
  Value() : data(nullptr) {}
  explicit Value(int64_t v) : data(v) {}
  explicit Value(std::string v) : data(std::move(v)) {}
  explicit Value(Array v) : data(std::move(v)) {}
  explicit Value(Object v) : data(std::move(v)) {}
  [[nodiscard]] bool is_object() const; [[nodiscard]] bool is_array() const; [[nodiscard]] bool is_string() const; [[nodiscard]] bool is_int() const;
  [[nodiscard]] const Object& object() const; [[nodiscard]] const Array& array() const; [[nodiscard]] const std::string& string() const; [[nodiscard]] int64_t integer() const;
  [[nodiscard]] const Value* find(const std::string& key) const;
};
Value parse(const std::string& text);
std::string canonical(const Value& value);
}
#endif
