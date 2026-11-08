#include "toml_lite.hpp"

#include "json_lite.hpp"  // formatRoundTrip

#include <charconv>
#include <cmath>
#include <cstdio>
#include <locale>
#include <sstream>

namespace skycued::toml_lite {

namespace {

bool isKeyChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    Parser(const std::string& text, const std::string& name) : s_(text), name_(name) {}

    Document run()
    {
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0)  // a UTF-8 byte order mark
            pos_ = 3;
        for (;;) {
            skipBlank();
            if (atEnd())
                break;
            if (peek() == '[')
                header();
            else
                assignment();
            endOfLine();
        }
        return std::move(doc_);
    }

private:
    [[noreturn]] void fail(const std::string& message) const { throw Error(name_, line_, message); }
    [[noreturn]] void failAt(int line, const std::string& message) const { throw Error(name_, line, message); }

    bool atEnd() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }
    bool isNewline() const { return peek() == '\n' || (peek() == '\r' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '\n'); }

    void newline()
    {
        if (peek() == '\r')
            ++pos_;
        ++pos_;
        ++line_;
    }

    void skipSpaces()
    {
        while (!atEnd() && (peek() == ' ' || peek() == '\t'))
            ++pos_;
    }

    // A '#' comment to the end of the line (not consuming the newline).
    void skipComment()
    {
        if (atEnd() || peek() != '#')
            return;
        while (!atEnd() && !isNewline()) {
            const unsigned char c = static_cast<unsigned char>(peek());
            if (c == '\r' || (c < 0x20 && c != '\t') || c == 0x7F)
                fail("control character in comment");
            ++pos_;
        }
    }

    // Blank space, comments and newlines, between statements.
    void skipBlank()
    {
        for (;;) {
            skipSpaces();
            skipComment();
            if (atEnd() || !isNewline())
                return;
            newline();
        }
    }

    // After a statement: spaces, an optional comment, then a newline or the end.
    void endOfLine()
    {
        skipSpaces();
        skipComment();
        if (atEnd())
            return;
        if (!isNewline())
            fail(peek() == '\r' ? "stray carriage return" : "unexpected text after the value");
        newline();
    }

    std::string bareKey(const char* what)
    {
        const std::size_t start = pos_;
        while (!atEnd() && isKeyChar(peek()))
            ++pos_;
        if (pos_ == start) {
            if (atEnd() || isNewline())
                fail(std::string("missing ") + what);
            if (peek() == '"' || peek() == '\'')
                fail("quoted keys are not supported");
            fail(std::string("invalid character in ") + what + " (letters, digits and _ only)");
        }
        return s_.substr(start, pos_ - start);
    }

    void header()
    {
        ++pos_;  // '['
        if (!atEnd() && peek() == '[')
            fail("arrays of tables are not supported");
        std::string name;
        for (;;) {
            skipSpaces();
            name += bareKey("table name");
            skipSpaces();
            if (atEnd() || isNewline())
                fail("unterminated table header");
            if (peek() == '.') {
                ++pos_;
                name += '.';
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            fail("invalid character in table header");
        }
        if (doc_.tables.count(name))
            fail("table [" + name + "] given twice");

        if (doc_.entries.count(name))
            fail("[" + name + "] is also a key");
        for (size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1))
            if (doc_.entries.count(name.substr(0, dot)))
                fail("[" + name + "]: " + name.substr(0, dot) + " is a key, not a table");
        doc_.tables.insert(name);
        doc_.table_lines[name] = line_;
        prefix_ = name + ".";
    }

    void assignment()
    {
        const int line = line_;
        const std::string key = bareKey("key");
        skipSpaces();
        if (atEnd() || isNewline())
            fail("missing '=' after key");
        if (peek() == '.')
            fail("dotted keys are not supported; write a [table] header");
        if (peek() != '=')
            fail("invalid character in key (letters, digits and _ only)");
        ++pos_;
        skipSpaces();
        if (atEnd() || isNewline() || peek() == '#')
            fail("missing value");
        const std::string full = prefix_ + key;
        if (doc_.entries.count(full))
            fail("key " + full + " given twice");
        if (doc_.tables.count(full))
            fail("key " + full + " is also a table");
        {  // a table below it ([a.b.c] seen, now b = 1 under [a]): the key's name is an implicit table
            const auto below = doc_.tables.lower_bound(full + ".");
            if (below != doc_.tables.end() && below->compare(0, full.size() + 1, full + ".") == 0)
                fail("key " + full + " is also a table");
        }
        Value v = value();
        doc_.entries.emplace(full, Entry{std::move(v), line});
    }

    Value value()
    {
        const char c = peek();
        if (c == '"' || c == '\'')
            return Value(std::in_place_type<std::string>, quoted());
        if (c == '[')
            return Value(std::in_place_type<Array>, array());
        if (c == '{')
            fail("inline tables are not supported");
        return scalar();
    }

    std::string quoted()
    {
        const char q = peek();
        if (s_.compare(pos_, 3, std::string(3, q)) == 0)
            fail("multi-line strings are not supported");
        ++pos_;
        std::string out;
        for (;;) {
            if (atEnd() || peek() == '\n' || peek() == '\r')
                fail("unterminated string");
            const char c = peek();
            ++pos_;
            if (c == q)
                return out;
            const unsigned char u = static_cast<unsigned char>(c);
            if ((u < 0x20 && u != '\t') || u == 0x7F)
                fail("control character in string");
            if (q == '\'' || c != '\\') {
                out += c;
                continue;
            }
            if (atEnd() || peek() == '\n' || peek() == '\r')
                fail("unterminated string");
            const char e = peek();
            ++pos_;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const int h = atEnd() ? -1 : hexValue(peek());
                    if (h < 0)
                        fail("\\u needs four hexadecimal digits");
                    cp = cp * 16 + static_cast<unsigned>(h);
                    ++pos_;
                }
                if (cp == 0)
                    fail("\\u0000 is not supported");
                if (cp >= 0xD800 && cp <= 0xDFFF)
                    fail("\\u escape is a surrogate, not a character");
                appendUtf8(out, cp);
                break;
            }
            default:
                fail(std::string("unsupported escape \\") + e);
            }
        }
    }

    Array array()
    {
        ++pos_;  // '['
        Array out;
        for (;;) {
            skipBlank();
            if (atEnd())
                fail("unterminated array");
            if (peek() == ']') {
                ++pos_;
                return out;
            }
            if (peek() != '"' && peek() != '\'') {
                if (peek() == '[')
                    fail("nested arrays are not supported");
                if (peek() == '{')
                    fail("inline tables are not supported");
                fail("arrays hold strings only");
            }
            out.push_back(quoted());
            skipBlank();
            if (atEnd())
                fail("unterminated array");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return out;
            }
            fail("expected ',' or ']' in array");
        }
    }

    // true, false, an integer or a float.
    Value scalar()
    {
        const std::size_t start = pos_;
        while (!atEnd() && peek() != ' ' && peek() != '\t' && peek() != '#' && peek() != '\n' && peek() != '\r' &&
               peek() != ',' && peek() != ']' && peek() != '}')
            ++pos_;
        const std::string tok = s_.substr(start, pos_ - start);
        if (tok == "true")
            return Value(std::in_place_type<bool>, true);
        if (tok == "false")
            return Value(std::in_place_type<bool>, false);
        if (looksLikeDateTime(tok))
            fail("dates and times are not supported");
        return number(tok);
    }

    static bool looksLikeDateTime(const std::string& t)
    {
        // 1979-05-27, 07:32:00, 1979-05-27T07:32:00Z
        if (t.size() >= 8 && isDigit(t[0]) && isDigit(t[1]) && isDigit(t[2]) && isDigit(t[3]) && t[4] == '-')
            return true;
        return t.size() >= 5 && isDigit(t[0]) && isDigit(t[1]) && t[2] == ':';
    }

    // Digits with single '_' between digits; returns the digits without '_'.
    // `ok` false when malformed.
    static bool digits(const std::string& t, std::size_t& i, std::string& out)
    {
        const std::size_t start = i;
        while (i < t.size() && (isDigit(t[i]) || t[i] == '_')) {
            if (t[i] == '_') {
                if (i == start || !isDigit(t[i - 1]) || i + 1 >= t.size() || !isDigit(t[i + 1]))
                    return false;
            } else {
                out += t[i];
            }
            ++i;
        }
        return i > start;
    }

    Value number(const std::string& tok)
    {
        if (tok.empty())
            fail("missing value");
        std::size_t i = 0;
        bool negative = false;
        if (tok[0] == '+' || tok[0] == '-') {
            negative = tok[0] == '-';
            ++i;
        }
        std::string intPart;
        if (!digits(tok, i, intPart))
            fail("invalid value '" + tok + "' (strings need quotes)");
        const bool leadingZero = intPart.size() > 1 && intPart[0] == '0';
        if (i == tok.size()) {
            if (leadingZero)
                fail("integer with a leading zero: " + tok);
            return integer(intPart, negative, tok);
        }
        std::string text = (negative ? "-" : "") + intPart;
        bool isFloat = false;
        if (tok[i] == '.') {
            ++i;
            std::string frac;
            if (!digits(tok, i, frac))
                fail("invalid number '" + tok + "'");
            text += "." + frac;
            isFloat = true;
        }
        if (i < tok.size() && (tok[i] == 'e' || tok[i] == 'E')) {
            ++i;
            std::string sign;
            if (i < tok.size() && (tok[i] == '+' || tok[i] == '-'))
                sign = tok[i++];
            std::string exp;
            if (!digits(tok, i, exp))
                fail("invalid number '" + tok + "'");
            text += "e" + sign + exp;
            isFloat = true;
        }
        if (!isFloat || i != tok.size())
            fail("invalid number '" + tok + "'");
        if (leadingZero)
            fail("number with a leading zero: " + tok);
        std::istringstream in(text);
        in.imbue(std::locale::classic());
        double d = 0;
        in >> d;
        if (in.fail() || !std::isfinite(d))
            fail("number out of range: " + tok);
        return Value(std::in_place_type<double>, d);
    }

    Value integer(const std::string& digitsOnly, bool negative, const std::string& tok)
    {
        uint64_t mag = 0;
        const char* end = digitsOnly.data() + digitsOnly.size();
        const auto r = std::from_chars(digitsOnly.data(), end, mag);
        const uint64_t limit = negative ? uint64_t{1} << 63 : (uint64_t{1} << 63) - 1;
        if (r.ec != std::errc() || r.ptr != end || mag > limit)
            fail("integer out of range: " + tok);
        const int64_t v = negative ? static_cast<int64_t>(~mag + 1) : static_cast<int64_t>(mag);
        return Value(std::in_place_type<int64_t>, v);
    }

    const std::string& s_;
    std::string name_;
    std::size_t pos_ = 0;
    int line_ = 1;
    std::string prefix_;
    Document doc_;
};

void appendQuoted(std::string& out, const std::string& s)
{
    out += '"';
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (u < 0x20 || u == 0x7F) {
                char buf[8];
                (void)std::snprintf(buf, sizeof buf, "\\u%04X", u);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

}  // namespace

Document parse(const std::string& text, const std::string& name_for_errors)
{
    return Parser(text, name_for_errors).run();
}

std::string formatValue(const Value& value)
{
    std::string out;
    if (const auto* s = std::get_if<std::string>(&value)) {
        appendQuoted(out, *s);
    } else if (const auto* i = std::get_if<int64_t>(&value)) {
        out = std::to_string(*i);
    } else if (const auto* d = std::get_if<double>(&value)) {
        if (!std::isfinite(*d))
            throw std::invalid_argument("toml_lite: cannot write a non-finite number");
        out = formatRoundTrip(*d);
        if (out.find_first_of(".eE") == std::string::npos)
            out += ".0";
    } else if (const auto* b = std::get_if<bool>(&value)) {
        out = *b ? "true" : "false";
    } else {
        out = "[";
        bool first = true;
        for (const auto& item : std::get<Array>(value)) {
            if (!first)
                out += ", ";
            first = false;
            appendQuoted(out, item);
        }
        out += "]";
    }
    return out;
}

}  // namespace skycued::toml_lite
