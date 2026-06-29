#pragma once
#include <functional>
#include <string>

namespace motion::http {

bool getString(const std::wstring& url, std::string& outBody, std::string& err,
               const std::wstring& extraHeaders = L"");

bool downloadFile(const std::wstring& url,
                  const std::wstring& destPath,
                  const std::function<void(unsigned long long received,
                                           unsigned long long total)>& onProgress,
                  std::string& err);

}
