#include "pbrenderd3d/data_window.h"

// Link the actual renderer without a Qt/Windows header, window, or GUI test.
int main()
{
    const pbrenderd3d::DataWindowConfig config;
    if (!pbrenderd3d::ValidateDataWindowConfig(config))
    {
        return 1;
    }
    const pbrenderd3d::CanonicalBgraFrameView empty;
    return pbrenderd3d::ValidateCanonicalBgraFrame(config, empty).code == pbrenderd3d::PresentationErrorCode::InvalidFrame ? 0 : 2;
}
