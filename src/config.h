#pragma once

#include <string>
#include <vector>

#include "hotkey.h"

struct FilterPreset
{
    std::string name;
    std::string expression;
    std::string pingTarget;
};

struct AppConfig
{
    std::string hotkey = "MOUSE5";
    std::string pingTarget;
    std::vector<FilterPreset> filters;
    std::string updateCheckUrl;
    std::string updatePageUrl;
};

[[nodiscard]] AppConfig makeDefaultConfig();

[[nodiscard]] bool loadConfigJson(const std::string &path, AppConfig &output,
                                  std::string &errorMessage);

[[nodiscard]] bool saveConfigJson(const std::string &path, const AppConfig &config,
                                  std::string &errorMessage);
