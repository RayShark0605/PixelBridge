#include "pbrenderd3d/data_window.h"

// Deliberately no PMv2 manifest: a library must reject this host before
// creating an HWND, rather than silently changing process DPI awareness.
int main()
{
    const auto result = pbrenderd3d::DataWindow::Create({});
    if (result)
    {
        return 1;
    }
    const auto status = result.Error();
    return status.code == pbrenderd3d::PresentationErrorCode::DpiAwarenessRequired && status.stage == pbrenderd3d::PresentationStage::DpiAwareness ? 0 : 2;
}
