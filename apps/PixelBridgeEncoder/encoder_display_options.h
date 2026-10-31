#pragma once

#include "application_model.h"
#include <optional>
#include <string_view>

namespace pbencoder
{

[[nodiscard]] inline std::optional<pbrenderd3d::PresentationModel> GetNextEncoderDisplayMode(const pbrenderd3d::PresentationModel model) noexcept
{
    if (model == pbrenderd3d::PresentationModel::Flip)
    {
        return pbrenderd3d::PresentationModel::QtRaster;
    }
    if (model == pbrenderd3d::PresentationModel::QtRaster)
    {
        return pbrenderd3d::PresentationModel::Flip;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool CanChangeEncoderDisplayMode(const pbapp::EncoderState state, const bool closePending,
    const bool fixedPresentation, const bool measurement) noexcept
{
    return !closePending && !fixedPresentation && !measurement &&
        (state == pbapp::EncoderState::Idle || state == pbapp::EncoderState::Stopped || state == pbapp::EncoderState::Failed);
}

[[nodiscard]] inline bool ParseGuiDisplayTestArguments(const int argumentCount, const wchar_t* const* arguments) noexcept
{
    return argumentCount == 2 && arguments != nullptr && arguments[1] != nullptr &&
        std::wstring_view(arguments[1]) == L"--gui-display-test";
}

[[nodiscard]] inline const char* GetEncoderDisplayModeLabel(const pbrenderd3d::PresentationModel model) noexcept
{
    switch (model)
    {
    case pbrenderd3d::PresentationModel::Flip: return "显示方式：D3D11（默认）";
    case pbrenderd3d::PresentationModel::QtRaster: return "显示方式：Qt 软件绘制（兼容）";
    case pbrenderd3d::PresentationModel::ComposedBlt: return "显示方式：D3D11 ComposedBlt（固定诊断）";
    }
    return "显示方式：未知（不可用）";
}

}
