#pragma once
#include "core/config.h"

namespace motion {

struct HwInfo {
    int  cores = 0;
    int  ramGB = 0;
    int  vramMB = 0;
    bool dedicatedGpu = false;
    int  tier = 1;
};

HwInfo scanHardware();
Quality recommendedQuality(const HwInfo& hw);
bool recommendLowEnd(const HwInfo& hw);
const char* tierName(const HwInfo& hw);

}
