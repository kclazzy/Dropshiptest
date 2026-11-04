#pragma once

#include <string>
#include <string_view>

namespace dropship {

// SHA-256 в нижнем регистре hex (64 символа).
std::string sha256Hex(std::string_view data);

}  // namespace dropship
