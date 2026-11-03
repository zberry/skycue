#pragma once

// The one JSON string escaper in the tree. Pure code: it is in
// skycued_asterix so skycue-dump (which links that library alone) and
// skycued_core share it; json_lite.hpp includes this header.

#include <string>

namespace skycued {

// Appends s to out as a JSON string literal, quotes included. '"' and '\'
// are escaped, control characters below 0x20 are \b \f \n \r \t or \u00XX,
// and the output is always valid UTF-8: a byte that does not start a
// well-formed RFC 3629 sequence (an overlong form, a surrogate, a value above
// U+10FFFF, a stray continuation byte) and every byte of a sequence cut short
// is written as U+FFFD, one per byte; valid sequences pass unchanged.
void appendJsonString(std::string& out, const std::string& s);

// True when s is well-formed UTF-8 (RFC 3629, the rule above): exactly the
// strings appendJsonString writes unchanged apart from its escapes.
bool isValidUtf8(const std::string& s);

}  // namespace skycued
