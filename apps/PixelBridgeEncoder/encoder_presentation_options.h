#pragma once

#include "pbrenderd3d/data_window.h"

#include <string_view>

namespace pbencoder
{

[[nodiscard]] inline std::string_view GetDiagnosticSourceFingerprint() noexcept
{
#ifdef PB_ENCODER_DIAGNOSTIC_SOURCE_FINGERPRINT
    return PB_ENCODER_DIAGNOSTIC_SOURCE_FINGERPRINT;
#else
    return {};
#endif
}

[[nodiscard]] inline bool ParseGuiPresentationModeArguments(const int argumentCount, const wchar_t* const* arguments,
    pbrenderd3d::PresentationModel& model) noexcept
{
    if (argumentCount != 3 || arguments == nullptr || arguments[1] == nullptr || arguments[2] == nullptr ||
        std::wstring_view(arguments[1]) != L"--gui-presentation-mode")
    {
        return false;
    }
    const std::wstring_view token(arguments[2]);
    if (token == L"flip" || token == L"composed-blt")
    {
        model = token == L"flip" ? pbrenderd3d::PresentationModel::Flip : pbrenderd3d::PresentationModel::ComposedBlt;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool ShouldRegisterEncoderShell(const bool diagnosticPresentation, const bool smoke,
    const bool nativeSmoke, const bool measurement, const bool integrationSmoke) noexcept
{
    return !diagnosticPresentation && !smoke && !nativeSmoke && !measurement && !integrationSmoke;
}

}
