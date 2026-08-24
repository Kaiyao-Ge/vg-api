#ifndef VG_IR_SHA256_H_
#define VG_IR_SHA256_H_
#include <string>
#include <string_view>
namespace vg::ir { std::string sha256_hex(std::string_view input); }
#endif
