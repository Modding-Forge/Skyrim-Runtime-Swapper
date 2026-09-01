#include <runtime_swapper/transaction_backend.hpp>

#include <string_view>

namespace runtime_swapper {
namespace {

[[nodiscard]] bool equal_ascii_ignore_case(std::wstring_view left,
                                           std::wstring_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto lower = [](wchar_t value) {
      return value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
    };
    if (lower(left[index]) != lower(right[index])) return false;
  }
  return true;
}

}  // namespace

SafetyMode classify_storage(const VolumeIdentity& target,
                            const VolumeIdentity& vault,
                            bool different_volume) noexcept {
  if (!target.local || !target.stable ||
      target.medium == StorageMedium::network || !vault.local ||
      !vault.stable || !vault.native_durability) {
    return SafetyMode::hard_blocked;
  }
  if (target.medium == StorageMedium::internal && target.native_durability) {
    return SafetyMode::automatic;
  }
  if (!different_volume) return SafetyMode::hard_blocked;
  if (target.medium == StorageMedium::external ||
      target.medium == StorageMedium::removable ||
      equal_ascii_ignore_case(target.filesystem, L"exfat")) {
    return SafetyMode::persistent_only;
  }
  return SafetyMode::persistent_with_warning;
}

std::wstring safety_mode_label(SafetyMode mode) {
  switch (mode) {
    case SafetyMode::automatic:
      return L"Automatic";
    case SafetyMode::persistent_only:
      return L"Persistent only";
    case SafetyMode::persistent_with_warning:
      return L"Persistent with warning";
    case SafetyMode::hard_blocked:
      return L"Hard blocked";
  }
  return L"Hard blocked";
}

}  // namespace runtime_swapper
