#pragma once

#include <string>

namespace skycued {

// PropertyTree's JSON parser recurses once per nesting level, so deep input
// would overflow the stack (a crash, not an exception). This linear scan finds
// the maximum {/[ depth outside string literals so such input is refused
// before read_json. Header-only: used by json_lite (skycued_core) and by
// capture_line (skycued_asterix, linked by the tools without the core).
inline bool nestingWithin(const std::string& text, int max_depth)
{
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\')
                ++i;  // skip the escaped character, so \" does not end the string
            else if (c == '"')
                in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth)
                return false;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return true;
}

}  // namespace skycued
