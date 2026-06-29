#pragma once
#include <string>
#include <vector>

namespace motion {

struct MonitorInfo {
    std::string  device;
    std::wstring deviceW;
    int  x = 0, y = 0;
    int  width = 0, height = 0;
    bool primary = false;
    int  index = 0;
};

std::vector<MonitorInfo> enumerateMonitors();

}
