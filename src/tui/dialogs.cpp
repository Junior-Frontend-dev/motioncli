#include "tui/dialogs.h"

#include <windows.h>
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

namespace motion::tui::dialogs {

namespace {
constexpr wchar_t kFilter[] =
    L"Video files\0*.mp4;*.wmv;*.avi;*.mov;*.mkv;*.m4v\0All files\0*.*\0";
}

std::wstring openVideoFile() {
    wchar_t path[MAX_PATH] = {0};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Choose a video to use as a wallpaper";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    return GetOpenFileNameW(&ofn) ? std::wstring(path) : std::wstring();
}

std::wstring saveVideoFile(const std::wstring& suggestedName) {
    wchar_t path[MAX_PATH] = {0};
    wcsncpy_s(path, suggestedName.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Export wallpaper video to…";
    ofn.lpstrDefExt = L"mp4";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    return GetSaveFileNameW(&ofn) ? std::wstring(path) : std::wstring();
}

}
