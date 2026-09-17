#pragma once
#include <string>
#include <string_view>

namespace bridge {
inline bool valid_id(std::string_view id) {
    if (id.size() != 32) return false;
    for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool valid_command(std::string_view value) {
    if (value.empty() || value.size() > 1023) return false;
    bool nonblank = false;
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) return false;
        nonblank |= c != ' ';
    }
    return nonblank;
}
inline std::string quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 0x20 || c >= 0x7f) {
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += c;
    }
    return out + '"';
}
}
