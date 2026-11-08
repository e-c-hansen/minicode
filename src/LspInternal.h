// LspInternal.h — small string helpers the core's Lsp*.cpp files share.
// Only those files include it; callers use LspClient.h.
#pragma once
#include <cctype>
#include <string>

namespace Lsp {

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

}  // namespace Lsp
