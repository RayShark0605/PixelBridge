#include "pbcore/build_info.h"

int main()
{
    const pbcore::BuildInfo buildInfo = pbcore::GetBuildInfo();
    if (buildInfo.productName != "PixelBridge" || buildInfo.version.empty())
    {
        return 1;
    }

    return 0;
}
