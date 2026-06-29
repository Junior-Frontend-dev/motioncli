#include "app/app.h"
#include "core/wallpaper.h"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv) {
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        LocalFree(argv);
    }

    for (const auto& a : args) {
        if (a == L"--render" || a == L"--startup")
            return motion::runEngineFromConfig();
    }

    motion::App app;
    return app.run();
}
