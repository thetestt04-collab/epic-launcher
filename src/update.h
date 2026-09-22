#pragma once

#include <string>
#include <vector>

struct UpdateFeed
{
    std::string checkUrl;
    std::string pageUrl;
};

struct UpdateCheckResult
{
    bool hasUpdate = false;
    bool automatic = false;
    std::string latestVersion;
    std::string downloadUrl;
    std::string error;
};

[[nodiscard]] std::string updateTrimVersion(const std::string &version);

[[nodiscard]] std::vector<int> updateParseVersionParts(const std::string &version);

[[nodiscard]] int updateCompareVersions(const std::string &a, const std::string &b);

[[nodiscard]] inline bool updateIsNewer(const std::string &current, const std::string &latest)
{
    return updateCompareVersions(current, latest) < 0;
}

[[nodiscard]] bool updateParseJson(const std::string &source, std::string &versionOut,
                                   std::string &urlOut);

[[nodiscard]] bool updateFetchHttpText(const std::string &url, std::string &output,
                                       std::string &errorMessage);

[[nodiscard]] UpdateCheckResult updateCheckForUpdates(const std::string &currentVersion,
                                                      const std::string &checkUrl,
                                                      const std::string &pageUrlFallback);

void updateOpenUrl(const std::string &url);
