#pragma once
#include <string>
#include <vector>

namespace motion::moewalls {

struct Item {
    std::string id;
    std::string title;
    std::string postUrl;
    std::string url;
    std::string preview;
    std::string previewVideo;
    std::string resolution;
    int width = 0;
    std::vector<std::string> tags;
};

bool fetchListing(const std::string& query, const std::string& category,
                  int maxItems, std::vector<Item>& out, std::string& err);

bool resolve(Item& item, std::string& err);

}
