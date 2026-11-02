#include "fisb/text_products.hpp"

#include "fisb/dlac.hpp"
#include "fisb/fisb_time.hpp"

#include <cctype>
#include <sstream>

// Report classification and the winds forecast-period matrix follow
// fisb-decode's level2/msg413.py (BSD-2-Clause Plus Patent, see NOTICE).

namespace skycued {
namespace fisb {

namespace {

bool isUpperAlnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool isStation(const std::string& t)
{
    if (t.size() < 3 || t.size() > 4)
        return false;
    bool letter = false;
    for (char c : t) {
        if (!isUpperAlnum(c))
            return false;
        letter = letter || (c >= 'A' && c <= 'Z');
    }
    return letter;
}

// "ddhhmmZ" -> (dd, hh, mm).
bool timeGroup(const std::string& t, unsigned& dd, unsigned& hh, unsigned& mm)
{
    if (t.size() != 7 || t[6] != 'Z')
        return false;
    for (int i = 0; i < 6; ++i)
        if (t[static_cast<std::size_t>(i)] < '0' || t[static_cast<std::size_t>(i)] > '9')
            return false;
    auto two = [&](std::size_t i) { return static_cast<unsigned>((t[i] - '0') * 10 + (t[i + 1] - '0')); };
    dd = two(0);
    hh = two(2);
    mm = two(4);
    return true;
}

// Removes trailing spaces from every line and trailing newlines from the end.
std::string clean(const std::string& s)
{
    std::string out;
    std::string line;
    for (char c : s) {
        if (c == '\n') {
            while (!line.empty() && line.back() == ' ')
                line.pop_back();
            out += line;
            out += '\n';
            line.clear();
        } else
            line += c;
    }
    while (!line.empty() && line.back() == ' ')
        line.pop_back();
    out += line;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    std::size_t lead = 0;
    while (lead < out.size() && (out[lead] == '\n' || out[lead] == ' '))
        ++lead;
    return out.substr(lead);
}

}  // namespace

std::optional<uint8_t> windsPeriod(unsigned apdu_hour, unsigned valid_hhmm)
{
    //   Prod avail | 0600 1200 1800 0000  <- valid times
    //       0200   |   6   12   --   24
    //       0800   |  24    6   12   --
    //       1400   |  --   24    6   12
    //       2000   |  12   --   24    6
    static const int matrix[4][4] = {{6, 12, -1, 24}, {24, 6, 12, -1}, {-1, 24, 6, 12}, {12, -1, 24, 6}};
    int pa;
    if (apdu_hour >= 1 && apdu_hour < 3)
        pa = 0;
    else if (apdu_hour >= 7 && apdu_hour < 9)
        pa = 1;
    else if (apdu_hour >= 13 && apdu_hour < 15)
        pa = 2;
    else if (apdu_hour >= 19 && apdu_hour < 21)
        pa = 3;
    else
        return std::nullopt;
    int vt;
    switch (valid_hhmm) {
    case 600: vt = 0; break;
    case 1200: vt = 1; break;
    case 1800: vt = 2; break;
    case 0: vt = 3; break;
    default: return std::nullopt;
    }
    const int v = matrix[pa][vt];
    if (v < 0)
        return std::nullopt;
    return static_cast<uint8_t>(v);
}

std::vector<TextRecord> parseTextProduct(const ApduHeader& h, const uint8_t* p, std::size_t n, TimePoint ref)
{
    std::vector<TextRecord> out;
    if (!p || n == 0)
        return out;
    const std::string all = decodeDlac(p, n);
    const auto apdu_t = apduTime(h, ref);

    std::size_t pos = 0;
    while (pos <= all.size()) {
        std::size_t rs = all.find('\x1e', pos);
        if (rs == std::string::npos)
            rs = all.size();
        std::string text = clean(all.substr(pos, rs - pos));
        pos = rs + 1;
        if (text.empty())
            continue;

        TextRecord rec;
        rec.text = text;
        std::istringstream first_line(text.substr(0, text.find('\n')));
        std::vector<std::string> tok;
        for (std::string t; first_line >> t;)
            tok.push_back(t);

        std::size_t i = 0;
        const std::string& w = tok.empty() ? text : tok[0];
        if (w == "METAR" || w == "SPECI")
            rec.kind = TextRecord::metar;
        else if (w == "TAF" || w.compare(0, 4, "TAF.") == 0) {
            rec.kind = TextRecord::taf;
            if (w == "TAF" && tok.size() > 1 && (tok[1] == "AMD" || tok[1] == "COR"))
                ++i;
        } else if (w == "PIREP" || w == "UA" || w == "UUA")
            rec.kind = TextRecord::pirep;
        else if (w == "WINDS")
            rec.kind = TextRecord::winds;
        if (rec.kind != TextRecord::other)
            ++i;

        for (std::size_t k = i; rec.kind != TextRecord::other && k < tok.size() && k < i + 2; ++k)
            if (isStation(tok[k])) {
                rec.station = tok[k];
                break;
            }

        std::optional<TimePoint> issued;
        for (std::size_t k = 0; k < tok.size() && k < 6 && !issued; ++k) {
            unsigned dd, hh, mm;
            if (timeGroup(tok[k], dd, hh, mm)) {
                issued = resolveUtc(ref, std::nullopt, dd, hh, mm);
                if (rec.kind == TextRecord::winds)
                    rec.period_h = windsPeriod(h.hours, hh * 100 + mm);
            }
        }
        if (rec.kind == TextRecord::winds) {
            // "WINDS SFO 301800Z": the group is the forecast valid time; the
            // product's issue time is the APDU time (reference §4).
            rec.valid = issued;
            rec.issued = apdu_t.value_or(ref);
        } else {
            rec.issued = issued ? *issued : apdu_t.value_or(ref);
        }
        out.push_back(std::move(rec));
    }
    return out;
}

}  // namespace fisb
}  // namespace skycued
