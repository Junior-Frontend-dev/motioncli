#pragma once
#include <string>

namespace motion {

class Config;

int runEngineFromConfig();

class EngineController {
public:
    explicit EngineController(Config& config) : m_config(config) {}

    bool restart(std::string& err);
    void stop();
    bool isRunning() const;

private:
    Config& m_config;
};

}
