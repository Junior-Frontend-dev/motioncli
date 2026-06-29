#pragma once
#include <string>

namespace motion::tui::dialogs {

std::wstring openVideoFile();
std::wstring saveVideoFile(const std::wstring& suggestedName);

}
