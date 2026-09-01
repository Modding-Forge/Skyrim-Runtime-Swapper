#pragma once

#include <limits>
#include <type_traits>

namespace runtime_swapper {

template <typename Integer>
[[nodiscard]] constexpr bool checked_add(Integer left, Integer right,
                                         Integer& result) noexcept {
  static_assert(std::is_unsigned_v<Integer>);
  if (right > (std::numeric_limits<Integer>::max)() - left) return false;
  result = static_cast<Integer>(left + right);
  return true;
}

template <typename Integer>
[[nodiscard]] constexpr bool checked_multiply(Integer left, Integer right,
                                              Integer& result) noexcept {
  static_assert(std::is_unsigned_v<Integer>);
  if (left != 0 && right > (std::numeric_limits<Integer>::max)() / left) {
    return false;
  }
  result = static_cast<Integer>(left * right);
  return true;
}

}  // namespace runtime_swapper
