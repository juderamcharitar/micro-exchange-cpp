#pragma once
#include <string>
#include <string_view>
namespace micro_exchange {
// SHA-256 digest of a byte string, lowercase hex. Used ONLY for local demo
// account capability verification; not a substitute for remote authentication.
std::string sha256_hex(std::string_view input);
}
