#pragma once

// skycued --convert-conf: an old skycue.conf (or skycue.overrides.conf) as
// skycue.toml text. One-shot migration; removed in a later release together
// with conf_legacy.

#include <string>

namespace skycued {

// The TOML equivalent of `old_text`, the old file `name` (named in errors).
// For a skycue.conf: the text of packaging/skycue.toml.example with the
// values of the old file filled in (its own comments are not carried over;
// *dropped_comment_lines, when given, is the number of comment lines that
// were). For a file that assigns only the keys of the old overrides file
// (overridesFileKeys()): an overrides-style file, a header line and the keys
// present. An old key that is repeated takes its last value, as it did.
// Throws ConfigError for a malformed line, an unknown key, or a value the old
// file would not have accepted (yes/no, numbers; the result is checked by the
// TOML loader too).
std::string convertOldConf(const std::string& old_text, const std::string& name, int* dropped_comment_lines = nullptr);

}  // namespace skycued
