#pragma once

class QString;

[[nodiscard]] int RunEncoderShellDialog(const QString& selectedPath, const QString& settingsFile);

[[nodiscard]] bool RunEncoderShellDialogSmoke();
