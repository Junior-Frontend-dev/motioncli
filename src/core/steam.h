#pragma once
#include <string>
#include <vector>

namespace motion::steam {

struct Item {
    std::string id;
    std::string title;
    std::string author;
    std::string preview;
};

bool fetchTrending(const std::string& sort, std::vector<Item>& out, std::string& err);

bool resolveVideoUrl(const std::string& id, std::wstring& outUrl, std::string& err);

}
