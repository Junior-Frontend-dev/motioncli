#pragma once
#include <string>

namespace motion::tui {

bool renderImage(const std::wstring& path, int maxCols, int maxRows, std::string& out);

}
