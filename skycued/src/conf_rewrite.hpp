#pragma once

// Rewrites values in skycue.conf text (the grammar of etc/skycue.conf.example)
// while keeping every other byte: comments, blank lines, the order of the
// keys, other keys' quoting, and the line endings.

#include <map>
#include <string>

namespace skycued {
namespace conf_rewrite {

// For each key in `values`: every assignment line of it ("KEY=" in the first
// column; the last one is the one that takes effect, so all are rewritten)
// gets the new value from '=' to the end of the line. The value is written
// unquoted when the grammar allows, else "double-quoted", else
// 'single-quoted'. Keys with no assignment line are appended at the end, in
// key order, after one "# added by skycue save-baseline" line (a newline is
// added first if the text does not end with one). New lines end in "\r\n"
// when the text's first line does, else "\n".
// Throws std::invalid_argument for a value no form can hold (one with both
// kinds of quote, or a line break).
std::string rewriteKeys(const std::string& text, const std::map<std::string, std::string>& values);

// `value` as it is written after "KEY=" (see above). Throws as above.
std::string quoteValue(const std::string& value);

}  // namespace conf_rewrite
}  // namespace skycued
