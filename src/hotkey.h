#pragma once

#include <string>
#include <vector>
#include <windows.h>

struct HotkeyBinding
{
    int vk = 0;
    unsigned modifiers = 0;
    std::string canonical;
};

[[nodiscard]] bool parseHotkeyString(const std::string &text, HotkeyBinding &output);

[[nodiscard]] bool hotkeyKeyName(int vk, std::string &name);

[[nodiscard]] const std::vector<int> &hotkeyCaptureKeys();

[[nodiscard]] bool hotkeyIsTypingKey(int vk) noexcept;
