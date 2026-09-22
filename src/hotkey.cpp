#include <cctype>
#include <string>
#include <vector>
#include <windows.h>

#include "hotkey.h"

namespace
{
struct HotkeyKeyEntry
{
    const char *name;
    int vk;
};

const HotkeyKeyEntry hotkeyKeyTable[] = {
    {"INSERT", VK_INSERT},   {"DELETE", VK_DELETE}, {"DEL", VK_DELETE},
    {"HOME", VK_HOME},       {"END", VK_END},       {"PAGEUP", VK_PRIOR},
    {"PGUP", VK_PRIOR},      {"PAGEDOWN", VK_NEXT}, {"PGDN", VK_NEXT},
    {"NUMLOCK", VK_NUMLOCK}, {"SCROLL", VK_SCROLL}, {"UP", VK_UP},
    {"DOWN", VK_DOWN},       {"LEFT", VK_LEFT},     {"RIGHT", VK_RIGHT},
    {"NUM0", VK_NUMPAD0},    {"NUM1", VK_NUMPAD1},  {"NUM2", VK_NUMPAD2},
    {"NUM3", VK_NUMPAD3},    {"NUM4", VK_NUMPAD4},  {"NUM5", VK_NUMPAD5},
    {"NUM6", VK_NUMPAD6},    {"NUM7", VK_NUMPAD7},  {"NUM8", VK_NUMPAD8},
    {"NUM9", VK_NUMPAD9},    {"NUMADD", VK_ADD},    {"NUMSUB", VK_SUBTRACT},
    {"NUMMUL", VK_MULTIPLY}, {"NUMDIV", VK_DIVIDE}, {"NUMDOT", VK_DECIMAL},
    {"DECIMAL", VK_DECIMAL},
    {"BACKSPACE", VK_BACK},  {"BACK", VK_BACK},     {"TAB", VK_TAB},
    {"ENTER", VK_RETURN},    {"RETURN", VK_RETURN}, {"PAUSE", VK_PAUSE},
    {"BREAK", VK_PAUSE},     {"CAPSLOCK", VK_CAPITAL}, {"CAPS", VK_CAPITAL},
    {"CAPITAL", VK_CAPITAL}, {"SPACE", VK_SPACE},   {"SPACEBAR", VK_SPACE},
    {"PRINTSCREEN", VK_SNAPSHOT}, {"PRTSC", VK_SNAPSHOT}, {"PRTSCR", VK_SNAPSHOT},
    {"SNAPSHOT", VK_SNAPSHOT},
    {"CLEAR", VK_CLEAR},     {"SELECT", VK_SELECT}, {"PRINT", VK_PRINT},
    {"EXECUTE", VK_EXECUTE}, {"HELP", VK_HELP},
    {"SEMICOLON", VK_OEM_1}, {"OEM_1", VK_OEM_1},
    {"EQUAL", VK_OEM_PLUS},  {"PLUS", VK_OEM_PLUS}, {"OEM_PLUS", VK_OEM_PLUS},
    {"COMMA", VK_OEM_COMMA}, {"OEM_COMMA", VK_OEM_COMMA},
    {"MINUS", VK_OEM_MINUS}, {"OEM_MINUS", VK_OEM_MINUS},
    {"PERIOD", VK_OEM_PERIOD}, {"OEM_PERIOD", VK_OEM_PERIOD},
    {"SLASH", VK_OEM_2},     {"OEM_2", VK_OEM_2},
    {"BACKQUOTE", VK_OEM_3}, {"GRAVE", VK_OEM_3},   {"OEM_3", VK_OEM_3},
    {"BRACKETLEFT", VK_OEM_4}, {"OEM_4", VK_OEM_4},
    {"BACKSLASH", VK_OEM_5}, {"OEM_5", VK_OEM_5},
    {"BRACKETRIGHT", VK_OEM_6}, {"OEM_6", VK_OEM_6},
    {"QUOTE", VK_OEM_7},     {"APOSTROPHE", VK_OEM_7}, {"OEM_7", VK_OEM_7},
    {"OEM_8", VK_OEM_8},     {"OEM_102", VK_OEM_102},
    {"BROWSER_BACK", VK_BROWSER_BACK},       {"BROWSER_FORWARD", VK_BROWSER_FORWARD},
    {"BROWSER_REFRESH", VK_BROWSER_REFRESH}, {"BROWSER_STOP", VK_BROWSER_STOP},
    {"BROWSER_SEARCH", VK_BROWSER_SEARCH},   {"BROWSER_FAVORITES", VK_BROWSER_FAVORITES},
    {"BROWSER_HOME", VK_BROWSER_HOME},
    {"VOLUME_MUTE", VK_VOLUME_MUTE},         {"VOLUME_DOWN", VK_VOLUME_DOWN},
    {"VOLUME_UP", VK_VOLUME_UP},
    {"MEDIA_NEXT_TRACK", VK_MEDIA_NEXT_TRACK}, {"MEDIA_NEXT", VK_MEDIA_NEXT_TRACK},
    {"MEDIA_PREV_TRACK", VK_MEDIA_PREV_TRACK}, {"MEDIA_PREV", VK_MEDIA_PREV_TRACK},
    {"MEDIA_STOP", VK_MEDIA_STOP},
    {"MEDIA_PLAY_PAUSE", VK_MEDIA_PLAY_PAUSE}, {"MEDIA_PLAY", VK_MEDIA_PLAY_PAUSE},
    {"LAUNCH_MAIL", VK_LAUNCH_MAIL},
    {"LAUNCH_MEDIA_SELECT", VK_LAUNCH_MEDIA_SELECT}, {"LAUNCH_MEDIA", VK_LAUNCH_MEDIA_SELECT},
    {"LAUNCH_APP1", VK_LAUNCH_APP1},         {"LAUNCH_APP2", VK_LAUNCH_APP2},
    {"SLEEP", VK_SLEEP},
};

bool lookupHotkeyKey(const std::string &name, int &vk, std::string &primary)
{
    for (const HotkeyKeyEntry &entry : hotkeyKeyTable)
    {
        if (name == entry.name)
        {
            vk = entry.vk;
            primary = entry.name;
            for (const HotkeyKeyEntry &first : hotkeyKeyTable)
            {
                if (first.vk == entry.vk)
                {
                    primary = first.name;
                    break;
                }
            }
            return true;
        }
    }
    return false;
}
} // namespace

bool parseHotkeyString(const std::string &text, HotkeyBinding &output)
{
    std::string upper;
    upper.reserve(text.size());
    for (char c : text)
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

    std::vector<std::string> parts;
    std::string part;
    for (char c : upper)
    {
        if (c == '+')
        {
            parts.push_back(part);
            part.clear();
        }
        else if (!std::isspace(static_cast<unsigned char>(c)))
        {
            part.push_back(c);
        }
    }
    parts.push_back(part);

    std::string key;
    for (const std::string &p : parts)
    {
        if (!p.empty())
            key = p;
    }
    if (key.empty())
        return false;

    unsigned modifiers = 0;
    for (const std::string &p : parts)
    {
        if (p.empty() || p == key)
            continue;
        if (p == "CTRL" || p == "CONTROL")
            modifiers |= MOD_CONTROL;
        else if (p == "ALT")
            modifiers |= MOD_ALT;
        else if (p == "SHIFT")
            modifiers |= MOD_SHIFT;
        else if (p == "WIN" || p == "WINDOWS" || p == "SUPER" || p == "META")
            modifiers |= MOD_WIN;
        else
            return false;
    }

    int vk = 0;
    std::string keyName;
    if (key == "MOUSE4")
    {
        vk = VK_XBUTTON1;
        keyName = "MOUSE4";
    }
    else if (key == "MOUSE5")
    {
        vk = VK_XBUTTON2;
        keyName = "MOUSE5";
    }
    else if (key.size() > 1 && key[0] == 'F')
    {
        int number = 0;
        for (std::size_t i = 1; i < key.size(); ++i)
        {
            if (key[i] < '0' || key[i] > '9')
                return false;
            number = number * 10 + (key[i] - '0');
        }
        if (number < 1 || number > 24)
            return false;
        vk = VK_F1 + (number - 1);
        keyName = "F" + std::to_string(number);
    }
    else if (key.size() == 1 && (std::isalpha(static_cast<unsigned char>(key[0])) ||
                                 std::isdigit(static_cast<unsigned char>(key[0]))))
    {
        vk = static_cast<unsigned char>(key[0]);
        keyName.push_back(key[0]);
    }
    else if (!lookupHotkeyKey(key, vk, keyName))
    {
        return false;
    }

    std::string canonical;
    if ((modifiers & MOD_CONTROL) != 0)
        canonical += "CTRL+";
    if ((modifiers & MOD_ALT) != 0)
        canonical += "ALT+";
    if ((modifiers & MOD_SHIFT) != 0)
        canonical += "SHIFT+";
    if ((modifiers & MOD_WIN) != 0)
        canonical += "WIN+";
    canonical += keyName;

    output.vk = vk;
    output.modifiers = modifiers;
    output.canonical = canonical;
    return true;
}

bool hotkeyKeyName(int vk, std::string &name)
{
    if (vk == VK_XBUTTON1)
    {
        name = "MOUSE4";
        return true;
    }
    if (vk == VK_XBUTTON2)
    {
        name = "MOUSE5";
        return true;
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        name = "F" + std::to_string(vk - VK_F1 + 1);
        return true;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        name = static_cast<char>(vk);
        return true;
    }
    for (const HotkeyKeyEntry &entry : hotkeyKeyTable)
    {
        if (entry.vk == vk)
        {
            name = entry.name;
            return true;
        }
    }
    return false;
}

const std::vector<int> &hotkeyCaptureKeys()
{
    static const std::vector<int> keys = []
    {
        std::vector<int> list;
        bool seen[256] = {false};
        auto push = [&](int vk)
        {
            if (vk < 0 || vk >= 256 || seen[vk])
                return;
            seen[vk] = true;
            list.push_back(vk);
        };
        push(VK_XBUTTON1);
        push(VK_XBUTTON2);
        for (int vk = VK_F1; vk <= VK_F24; ++vk)
            push(vk);
        for (int vk = 'A'; vk <= 'Z'; ++vk)
            push(vk);
        for (int vk = '0'; vk <= '9'; ++vk)
            push(vk);
        for (const HotkeyKeyEntry &entry : hotkeyKeyTable)
            push(entry.vk);
        push(VK_LWIN);
        push(VK_RWIN);
        return list;
    }();
    return keys;
}

bool hotkeyIsTypingKey(int vk) noexcept
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return true;
    if (vk == VK_SPACE || vk == VK_RETURN || vk == VK_TAB || vk == VK_BACK || vk == VK_CAPITAL)
        return true;
    if ((vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || vk == VK_ADD || vk == VK_SUBTRACT ||
        vk == VK_MULTIPLY || vk == VK_DIVIDE || vk == VK_DECIMAL || vk == VK_CLEAR)
        return true;
    if (vk >= VK_OEM_1 && vk <= VK_OEM_102)
        return true;
    return false;
}
