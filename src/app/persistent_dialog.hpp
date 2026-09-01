#pragma once

#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>

namespace runtime_swapper::app {

enum class PersistentDialogChoice { accepted, cancelled };

[[nodiscard]] PersistentDialogChoice show_persistent_downgrade_dialog(
    const std::filesystem::path& game_root, const BackendProbeResult& probe);
void show_hard_blocked_dialog(const BackendProbeResult& probe);

}  // namespace runtime_swapper::app
