#pragma once

class QString;

namespace pbencoder
{

enum class ShellDialogCancelAction
{
    Reject,
    RequestStop
};

[[nodiscard]] constexpr ShellDialogCancelAction GetShellDialogCancelAction(const bool active) noexcept
{
    return active ? ShellDialogCancelAction::RequestStop : ShellDialogCancelAction::Reject;
}

} // namespace pbencoder

[[nodiscard]] int RunEncoderShellDialog(const QString& selectedPath, const QString& settingsFile);

[[nodiscard]] bool RunEncoderShellDialogSmoke();
