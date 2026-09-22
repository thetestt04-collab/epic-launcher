#include <cctype>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include "update.h"

namespace
{
constexpr DWORD UPDATE_CONNECT_TIMEOUT_MS = 5000;
constexpr DWORD UPDATE_SEND_TIMEOUT_MS = 5000;
constexpr DWORD UPDATE_RECEIVE_TIMEOUT_MS = 5000;
constexpr std::size_t UPDATE_MAX_BYTES = 65536;

void trimInPlace(std::string &value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
        ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        --end;
    if (begin != 0 || end != value.size())
        value = value.substr(begin, end - begin);
}

bool findJsonStringField(const std::string &source, const std::string &field, std::string &out)
{
    const std::string key = "\"" + field + "\"";
    std::size_t pos = source.find(key);
    if (pos == std::string::npos)
        return false;
    pos = source.find(':', pos + key.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos])) != 0)
        ++pos;
    if (pos >= source.size() || source[pos] != '"')
        return false;
    ++pos;
    std::string value;
    while (pos < source.size())
    {
        const char c = source[pos++];
        if (c == '"')
        {
            out = value;
            return true;
        }
        if (c == '\\' && pos < source.size())
        {
            const char esc = source[pos++];
            switch (esc)
            {
                case '"':
                    value.push_back('"');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case '/':
                    value.push_back('/');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    value.push_back(esc);
                    break;
            }
        }
        else
        {
            value.push_back(c);
        }
        if (value.size() > UPDATE_MAX_BYTES)
            return false;
    }
    return false;
}

std::wstring toWide(const std::string &narrow)
{
    if (narrow.empty())
        return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(),
                                           static_cast<int>(narrow.size()), nullptr, 0);
    if (needed <= 0)
        return std::wstring();
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), static_cast<int>(narrow.size()), wide.data(),
                        needed);
    return wide;
}
} // namespace

std::string updateTrimVersion(const std::string &version)
{
    std::string value = version;
    trimInPlace(value);
    if (!value.empty() && (value[0] == 'v' || value[0] == 'V'))
        value.erase(0, 1);
    trimInPlace(value);
    return value;
}

std::vector<int> updateParseVersionParts(const std::string &version)
{
    std::vector<int> parts;
    const std::string cleaned = updateTrimVersion(version);
    std::string current;
    const auto flush = [&]()
    {
        if (current.empty())
        {
            parts.push_back(0);
            return;
        }
        int number = 0;
        for (char c : current)
        {
            if (c < '0' || c > '9')
                break;
            number = number * 10 + (c - '0');
        }
        parts.push_back(number);
        current.clear();
    };

    for (char c : cleaned)
    {
        if (c == '.')
        {
            flush();
        }
        else if (c == '-' || c == '_' || c == '+')
        {
            flush();
            break;
        }
        else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        {
            current.push_back(c);
        }
        else
        {
            flush();
            break;
        }
    }
    flush();
    return parts;
}

int updateCompareVersions(const std::string &a, const std::string &b)
{
    const std::string cleanA = updateTrimVersion(a);
    const std::string cleanB = updateTrimVersion(b);
    const std::vector<int> partsA = updateParseVersionParts(cleanA);
    const std::vector<int> partsB = updateParseVersionParts(cleanB);
    const std::size_t count = partsA.size() > partsB.size() ? partsA.size() : partsB.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const int partA = i < partsA.size() ? partsA[i] : 0;
        const int partB = i < partsB.size() ? partsB[i] : 0;
        if (partA < partB)
            return -1;
        if (partA > partB)
            return 1;
    }
    const bool preA = cleanA.find('-') != std::string::npos;
    const bool preB = cleanB.find('-') != std::string::npos;
    if (preA && !preB)
        return -1;
    if (!preA && preB)
        return 1;
    return 0;
}

bool updateParseJson(const std::string &source, std::string &versionOut, std::string &urlOut)
{
    versionOut.clear();
    urlOut.clear();
    std::string version;
    if (!findJsonStringField(source, "version", version) &&
        !findJsonStringField(source, "tag_name", version))
        return false;
    std::string url;
    if (!findJsonStringField(source, "html_url", url))
        findJsonStringField(source, "url", url);
    versionOut = updateTrimVersion(version);
    urlOut = url;
    trimInPlace(urlOut);
    return !versionOut.empty();
}

bool updateFetchHttpText(const std::string &url, std::string &output, std::string &errorMessage)
{
    output.clear();
    errorMessage.clear();
    if (url.empty() || url.size() > 2048)
    {
        errorMessage = "Update check URL is empty or too long.";
        return false;
    }

    const std::wstring wideUrl = toWide(url);
    if (wideUrl.empty())
    {
        errorMessage = "Update check URL is not valid UTF-8.";
        return false;
    }

    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts) == FALSE)
    {
        errorMessage = "Update check URL could not be parsed.";
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
    {
        errorMessage = "Only http(s) update URLs are supported.";
        return false;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(L"/");
    if (parts.dwUrlPathLength > 0)
        path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    HINTERNET session = WinHttpOpen(L"EpicGamesLauncher-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        errorMessage = "Could not start the HTTP session.";
        return false;
    }
    WinHttpSetTimeouts(session, UPDATE_CONNECT_TIMEOUT_MS, UPDATE_CONNECT_TIMEOUT_MS,
                       UPDATE_SEND_TIMEOUT_MS, UPDATE_RECEIVE_TIMEOUT_MS);

    bool ok = false;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    const INTERNET_PORT port =
        parts.nPort != 0 ? parts.nPort
                         : (parts.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_DEFAULT_HTTPS_PORT
                                                                   : INTERNET_DEFAULT_HTTP_PORT);
    connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (connection == nullptr)
    {
        errorMessage = "Could not connect to the update server.";
    }
    else
    {
        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (request == nullptr)
        {
            errorMessage = "Could not create the update request.";
        }
        else if (WinHttpSendRequest(request, L"Accept: application/json\r\n", 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE)
        {
            errorMessage = "Failed to send the update request.";
        }
        else if (WinHttpReceiveResponse(request, nullptr) == FALSE)
        {
            errorMessage = "No response from the update server.";
        }
        else
        {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                    WINHTTP_NO_HEADER_INDEX) == FALSE ||
                status != 200)
            {
                errorMessage = "Update server returned an error.";
            }
            else
            {
                std::string body;
                DWORD available = 0;
                for (;;)
                {
                    if (WinHttpQueryDataAvailable(request, &available) == FALSE)
                    {
                        errorMessage = "Failed while reading the update response.";
                        break;
                    }
                    if (available == 0)
                    {
                        ok = true;
                        break;
                    }
                    if (body.size() + available > UPDATE_MAX_BYTES)
                    {
                        errorMessage = "Update response is too large.";
                        break;
                    }
                    const std::size_t offset = body.size();
                    body.resize(offset + available);
                    DWORD read = 0;
                    if (WinHttpReadData(request, body.data() + offset, available, &read) == FALSE)
                    {
                        errorMessage = "Failed while reading the update response.";
                        break;
                    }
                    body.resize(offset + read);
                    if (read == 0)
                    {
                        ok = true;
                        break;
                    }
                }
                if (ok)
                    output = body;
            }
        }
    }

    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connection != nullptr)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok)
        output.clear();
    return ok;
}

UpdateCheckResult updateCheckForUpdates(const std::string &currentVersion,
                                        const std::string &checkUrl,
                                        const std::string &pageUrlFallback)
{
    UpdateCheckResult result;
    if (checkUrl.empty())
    {
        result.error = "Update checks are disabled (no updateCheckUrl configured).";
        return result;
    }

    std::string body;
    std::string fetchError;
    if (!updateFetchHttpText(checkUrl, body, fetchError))
    {
        result.error = fetchError;
        return result;
    }

    std::string latest;
    std::string url;
    if (!updateParseJson(body, latest, url))
    {
        result.error = "Update response did not contain a version.";
        return result;
    }

    result.latestVersion = latest;
    result.downloadUrl = !url.empty() ? url : pageUrlFallback;
    result.hasUpdate = updateIsNewer(currentVersion, latest);
    return result;
}

void updateOpenUrl(const std::string &url)
{
    if (url.empty())
        return;
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
