#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config.h"

namespace
{
constexpr std::size_t MAX_CONFIG_BYTES = 64 * 1024;
constexpr std::size_t MAX_FILTERS = 64;

class JsonParser final
{
public:
    explicit JsonParser(std::string_view source) : source_(source) {}

    [[nodiscard]] bool parse(AppConfig &config)
    {
        skipWhitespace();
        if (!consume('{'))
            return fail("expected a JSON object");

        bool hasFilters = false;
        skipWhitespace();
        while (!consume('}'))
        {
            std::string key;
            if (!parseString(key))
                return false;
            if (!expect(':'))
                return false;

            if (key == "hotkey")
            {
                if (!parseString(config.hotkey))
                    return false;
            }
            else if (key == "pingTarget")
            {
                if (!parseString(config.pingTarget))
                    return false;
            }
            else if (key == "filters")
            {
                if (!parseFilters(config.filters))
                    return false;
                hasFilters = true;
            }
            else if (key == "updateCheckUrl")
            {
                if (!parseString(config.updateCheckUrl))
                    return false;
            }
            else if (key == "updatePageUrl")
            {
                if (!parseString(config.updatePageUrl))
                    return false;
            }
            else if (!skipValue())
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
                break;
            if (!expect(','))
                return false;
        }

        skipWhitespace();
        if (position_ != source_.size())
            return fail("unexpected content after the root object");
        if (!hasFilters || config.filters.empty())
            return fail("filters must contain at least one preset");
        if (config.hotkey.empty())
            return fail("hotkey cannot be empty");
        if (config.pingTarget.empty())
            return fail("pingTarget cannot be empty");
        return true;
    }

    [[nodiscard]] const std::string &error() const noexcept { return error_; }

private:
    [[nodiscard]] bool parseFilters(std::vector<FilterPreset> &filters)
    {
        if (!expect('['))
            return false;

        filters.clear();
        skipWhitespace();
        while (!consume(']'))
        {
            if (filters.size() >= MAX_FILTERS)
                return fail("filters contains more than 64 presets");
            if (!expect('{'))
                return false;

            FilterPreset preset;
            skipWhitespace();
            while (!consume('}'))
            {
                std::string key;
                if (!parseString(key) || !expect(':'))
                    return false;

                if (key == "name")
                {
                    if (!parseString(preset.name))
                        return false;
                }
                else if (key == "expression")
                {
                    if (!parseString(preset.expression))
                        return false;
                }
                else if (key == "pingTarget")
                {
                    if (!parseString(preset.pingTarget))
                        return false;
                }
                else if (!skipValue())
                {
                    return false;
                }

                skipWhitespace();
                if (consume('}'))
                    break;
                if (!expect(','))
                    return false;
            }

            if (preset.name.empty() || preset.expression.empty())
                return fail("every filter requires non-empty name and expression fields");
            filters.push_back(std::move(preset));

            skipWhitespace();
            if (consume(']'))
                break;
            if (!expect(','))
                return false;
        }
        return true;
    }

    [[nodiscard]] bool parseString(std::string &value)
    {
        skipWhitespace();
        if (!consume('"'))
            return fail("expected a string");

        value.clear();
        while (position_ < source_.size())
        {
            const unsigned char character = static_cast<unsigned char>(source_[position_++]);
            if (character == '"')
                return true;
            if (character < 0x20)
                return fail("control character in string");
            if (character != '\\')
            {
                value.push_back(static_cast<char>(character));
                continue;
            }

            if (position_ >= source_.size())
                return fail("unterminated string escape");
            const char escaped = source_[position_++];
            switch (escaped)
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
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
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
                case 'u':
                {
                    std::uint32_t codePoint = 0;
                    if (!parseHexCodeUnit(codePoint))
                        return false;
                    appendUtf8(value, codePoint);
                    break;
                }
                default:
                    return fail("invalid string escape");
            }
        }
        return fail("unterminated string");
    }

    [[nodiscard]] bool parseHexCodeUnit(std::uint32_t &value)
    {
        if (source_.size() - position_ < 4)
            return fail("incomplete Unicode escape");
        value = 0;
        for (int index = 0; index < 4; ++index)
        {
            const char character = source_[position_++];
            value <<= 4;
            if (character >= '0' && character <= '9')
                value += static_cast<std::uint32_t>(character - '0');
            else if (character >= 'a' && character <= 'f')
                value += static_cast<std::uint32_t>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F')
                value += static_cast<std::uint32_t>(character - 'A' + 10);
            else
                return fail("invalid Unicode escape");
        }
        return true;
    }

    static void appendUtf8(std::string &output, std::uint32_t codePoint)
    {
        if (codePoint <= 0x7F)
        {
            output.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FF)
        {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    [[nodiscard]] bool skipValue()
    {
        skipWhitespace();
        if (position_ >= source_.size())
            return fail("expected a value");

        if (source_[position_] == '"')
        {
            std::string ignored;
            return parseString(ignored);
        }
        if (source_[position_] == '{')
        {
            ++position_;
            skipWhitespace();
            while (!consume('}'))
            {
                std::string ignored;
                if (!parseString(ignored) || !expect(':') || !skipValue())
                    return false;
                skipWhitespace();
                if (consume('}'))
                    break;
                if (!expect(','))
                    return false;
            }
            return true;
        }
        if (source_[position_] == '[')
        {
            ++position_;
            skipWhitespace();
            while (!consume(']'))
            {
                if (!skipValue())
                    return false;
                skipWhitespace();
                if (consume(']'))
                    break;
                if (!expect(','))
                    return false;
            }
            return true;
        }

        const std::size_t start = position_;
        while (position_ < source_.size())
        {
            const char character = source_[position_];
            if (std::isspace(static_cast<unsigned char>(character)) || character == ',' ||
                character == ']' || character == '}')
                break;
            ++position_;
        }
        if (position_ == start)
            return fail("invalid value");
        return true;
    }

    void skipWhitespace() noexcept
    {
        while (position_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[position_])))
            ++position_;
    }

    [[nodiscard]] bool consume(char expected) noexcept
    {
        skipWhitespace();
        if (position_ < source_.size() && source_[position_] == expected)
        {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool expect(char expected)
    {
        if (consume(expected))
            return true;
        std::string message = "expected '";
        message.push_back(expected);
        message.push_back('\'');
        return fail(message);
    }

    [[nodiscard]] bool fail(const std::string &message)
    {
        if (error_.empty())
            error_ = message + " at byte " + std::to_string(position_);
        return false;
    }

    std::string_view source_;
    std::size_t position_ = 0;
    std::string error_;
};
} // namespace

AppConfig makeDefaultConfig()
{
    AppConfig config;
    config.filters = {
        {"game", "outbound", ""},
        {"All sending packets", "outbound", ""},
        {"All receiving packets", "inbound", ""},
    };
    return config;
}

bool loadConfigJson(const std::string &path, AppConfig &output, std::string &errorMessage)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        errorMessage = "Could not open " + path;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0 || fileSize > static_cast<std::streamoff>(MAX_CONFIG_BYTES))
    {
        errorMessage = "Configuration must be smaller than 64 KiB";
        return false;
    }
    file.seekg(0, std::ios::beg);

    std::ostringstream contents;
    contents << file.rdbuf();
    if (!file.good() && !file.eof())
    {
        errorMessage = "Failed while reading " + path;
        return false;
    }

    AppConfig parsed;
    const std::string source = contents.str();
    JsonParser parser(source);
    if (!parser.parse(parsed))
    {
        errorMessage = "Invalid config.json: " + parser.error();
        return false;
    }

    output = std::move(parsed);
    errorMessage.clear();
    return true;
}

namespace
{
void appendJsonString(std::string &out, const std::string &value)
{
    static const char *hex = "0123456789abcdef";
    out.push_back('"');
    for (char c : value)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (u < 0x20)
                {
                    out += "\\u00";
                    out.push_back(hex[(u >> 4) & 0xF]);
                    out.push_back(hex[u & 0xF]);
                }
                else
                {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}
} // namespace

bool saveConfigJson(const std::string &path, const AppConfig &config, std::string &errorMessage)
{
    std::string out = "{\n";
    out += "  \"hotkey\": ";
    appendJsonString(out, config.hotkey);
    out += ",\n  \"pingTarget\": ";
    appendJsonString(out, config.pingTarget);
    out += ",\n  \"updateCheckUrl\": ";
    appendJsonString(out, config.updateCheckUrl);
    out += ",\n  \"updatePageUrl\": ";
    appendJsonString(out, config.updatePageUrl);
    out += ",\n  \"filters\": [\n";
    for (std::size_t i = 0; i < config.filters.size(); ++i)
    {
        out += "    {\n      \"name\": ";
        appendJsonString(out, config.filters[i].name);
        out += ",\n      \"expression\": ";
        appendJsonString(out, config.filters[i].expression);
        out += ",\n      \"pingTarget\": ";
        appendJsonString(out, config.filters[i].pingTarget);
        out += "\n    }";
        if (i + 1 < config.filters.size())
            out += ",";
        out += "\n";
    }
    out += "  ]\n}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        errorMessage = "Could not open " + path + " for writing";
        return false;
    }
    file << out;
    file.flush();
    if (!file)
    {
        errorMessage = "Failed while writing " + path;
        return false;
    }
    errorMessage.clear();
    return true;
}
