#include "core/hardware.h"

#include <windows.h>
#include <dxgi.h>

#pragma comment(lib, "dxgi.lib")

namespace motion {

namespace {

int detectVramMB() {
    int best = 0;
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)) || !factory)
        return 0;

    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            if (!wcsstr(desc.Description, L"Basic Render")) {
                int mb = (int)(desc.DedicatedVideoMemory / (1024 * 1024));
                if (mb > best) best = mb;
            }
        }
        adapter->Release();
    }
    factory->Release();
    return best;
}

}

HwInfo scanHardware() {
    HwInfo hw;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    hw.cores = (int)si.dwNumberOfProcessors;

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
        hw.ramGB = (int)(mem.ullTotalPhys / (1024ull * 1024 * 1024));

    hw.vramMB = detectVramMB();
    hw.dedicatedGpu = hw.vramMB >= 1024;

    if (hw.cores >= 8 && hw.ramGB >= 16 && hw.vramMB >= 3000)
        hw.tier = 2;
    else if (hw.cores >= 4 && hw.ramGB >= 8)
        hw.tier = 1;
    else
        hw.tier = 0;

    return hw;
}

Quality recommendedQuality(const HwInfo& hw) {
    if (hw.tier >= 2) return Quality::High;
    if (hw.tier == 1) return Quality::Medium;
    return Quality::Low;
}

bool recommendLowEnd(const HwInfo& hw) {
    return hw.tier == 0 || !hw.dedicatedGpu;
}

const char* tierName(const HwInfo& hw) {
    return hw.tier >= 2 ? "high-end" : hw.tier == 1 ? "mid-range" : "low-end";
}

}
