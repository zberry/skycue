// Test-only generator of the synthetic UAT coverage matrix (tests/fixtures/gen_uat_synthetic.sh).
//
//   uat_synth [--expect FILE]
//
// stdout: one "-HEX;" line per matrix row, built with encodeUat (the inverse of our layout
// table). FILE (JSONL, one object per row, keyed by "hex"): the values built into the frame, in
// the key names of the dump978-fa golden (gen_uat_golden.py), so the three sides compare field
// by field. A key is absent where the row builds "not available". Extra keys: row, group, and
// for ground rows track_type and heading (the built-in track/heading, never a "track").
// Every row has its own address, so no two frames are equal.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "uat_test_support.hpp"

using namespace skycued::testing;

namespace {

struct Row {
    std::string group, name;
    UatSpec spec;
    bool squawk_row = false;  // csid 0: call holds the four octal digits then padding
};

std::vector<Row> g_rows;

// Airborne baseline: positioned, 5000 ft barometric, 120 kt north / 50 kt west, +640 fpm
// barometric, category A0, call sign TEST, NIC 9, NACp 9, no emergency, no secondary altitude.
UatSpec baseline()
{
    UatSpec s;
    s.type = 1;
    s.lat = 37.5;
    s.lon = -122.25;
    s.alt_ft = 5000;
    s.nic = 9;
    s.ag = 0;
    s.ns_kt = 120;
    s.ew_kt = -50;
    s.vrate_fpm = 640;
    s.emitter = 0;
    s.call = "TEST    ";
    s.csid = true;
    s.emergency = 0;
    s.nacp = 9;
    return s;
}

Row& add(const std::string& group, const std::string& name, const UatSpec& spec)
{
    Row r;
    r.group = group;
    r.name = name;
    r.spec = spec;
    r.spec.address = 0x100000u + static_cast<uint32_t>(g_rows.size());
    g_rows.push_back(r);
    return g_rows.back();
}

void buildMatrix()
{
    for (unsigned c = 0; c < 40; ++c) {
        UatSpec s = baseline();
        s.emitter = c;
        add("category", "category" + std::to_string(c), s);
    }
    for (unsigned e = 0; e < 8; ++e) {
        UatSpec s = baseline();
        s.emergency = e;
        add("emergency", "emergency" + std::to_string(e), s);
    }
    for (unsigned t = 0; t < 4; ++t) {
        UatSpec s = baseline();
        s.ag = 2;
        s.ground_speed_kt = 25;
        s.track_type = t;
        s.track_deg = 123.0;
        s.ns_kt.reset();
        s.ew_kt.reset();
        s.vrate_fpm.reset();
        add("ground", "ground_tracktype" + std::to_string(t), s);
    }
    {
        UatSpec s = baseline();
        s.ag = 2;
        s.ground_speed_kt.reset();
        s.track_type = 1;
        s.track_deg = 123.0;
        s.ns_kt.reset();
        s.ew_kt.reset();
        s.vrate_fpm.reset();
        add("ground", "ground_speed_na", s);
    }
    {
        UatSpec s = baseline();
        s.zero_position = true;
        s.nic = 0;
        add("position", "airborne_no_position", s);
    }
    {
        UatSpec s = baseline();
        s.ns_kt.reset();
        add("velocity", "airborne_ns_na", s);
        s = baseline();
        s.ew_kt.reset();
        add("velocity", "airborne_ew_na", s);
        s = baseline();
        s.ag = 1;
        s.ns_kt = 480;
        s.ew_kt = -120;
        add("velocity", "supersonic_extra", s);
        s = baseline();
        s.ns_kt = -80;
        s.ew_kt = 90;
        add("velocity", "south_east", s);
    }
    {
        UatSpec s = baseline();
        s.vrate_fpm = 640;
        s.vrate_geometric = false;
        add("vrate", "vrate_baro_up", s);
        s.vrate_fpm = -1280;
        add("vrate", "vrate_baro_down", s);
        s.vrate_geometric = true;
        s.vrate_fpm = 960;
        add("vrate", "vrate_geom_up", s);
        s.vrate_fpm = -1920;
        add("vrate", "vrate_geom_down", s);
        s.vrate_fpm.reset();
        add("vrate", "vrate_na", s);
    }
    {
        UatSpec s = baseline();
        s.csid = false;
        s.call = "1200????";
        s.emitter = 1;
        add("squawk", "squawk_1200", s).squawk_row = true;
        s = baseline();
        s.csid = true;
        s.call = "N123AB  ";
        s.emitter = 1;
        add("squawk", "callsign_n123ab", s);
    }
    {
        for (int geom = 0; geom < 2; ++geom) {
            for (int present = 1; present >= 0; --present) {
                UatSpec s = baseline();
                s.alt_geometric = geom != 0;
                s.alt_ft = geom ? 5250 : 5000;
                if (present)
                    s.aux_alt_ft = geom ? 5000 : 5250;  // the other kind of altitude
                add("aux", std::string("aux_primary_") + (geom ? "geom" : "baro") + (present ? "_with" : "_without"), s);
            }
        }
    }
    for (unsigned q = 0; q < 7; ++q) {
        UatSpec s = baseline();
        s.qualifier = q;
        add("qualifier", "qualifier" + std::to_string(q), s);
    }
    {
        const struct { const char* n; double lat, lon; } p[] = {
            {"ne_corner", 89.9, 179.9},       {"sw_corner", -89.9, -179.9},    {"nw_corner", 89.9, -179.9},
            {"se_corner", -89.9, 179.9},      {"origin", 0.0, 0.0},            {"near_origin", -0.0005, 0.0005}};
        for (const auto& q : p) {
            UatSpec s = baseline();
            s.lat = q.lat;
            s.lon = q.lon;
            add("latlon", std::string("latlon_") + q.n, s);
        }
    }
    for (int alt : {-1000, 0, 101350}) {
        UatSpec s = baseline();
        s.alt_ft = alt;
        add("altitude", "altitude_" + std::to_string(alt), s);
    }
    for (unsigned n = 0; n < 12; ++n) {
        UatSpec s = baseline();
        s.nic = n;
        add("nic", "nic" + std::to_string(n), s);
    }
    for (unsigned n = 0; n < 12; ++n) {
        UatSpec s = baseline();
        s.nacp = n;
        add("nacp", "nacp" + std::to_string(n), s);
    }
}

std::string num(double v)
{
    char b[48];
    const int n = std::snprintf(b, sizeof b, "%.7g", v);
    return n > 0 ? std::string(b) : std::string("0");
}

// The built-in values of one row, as a JSON object. Fields the row builds as not available are
// left out; fields that cannot exist in the frame type (mode status in a state-vector-only frame)
// as well.
std::string expectJson(const Row& r, const std::string& hex)
{
    const UatSpec& s = r.spec;
    std::string o = "{\"hex\":\"" + hex + "\",\"group\":\"" + r.group + "\",\"row\":\"" + r.name + "\"";
    auto kv = [&](const char* k, const std::string& v) { o += std::string(",\"") + k + "\":" + v; };
    auto str = [&](const std::string& v) { return "\"" + v + "\""; };
    char ab[8];
    if (std::snprintf(ab, sizeof ab, "%06x", static_cast<unsigned>(s.address)) < 0) ab[0] = '\0';
    kv("address", str(ab));
    kv("qualifier_code", std::to_string(s.qualifier));
    kv("ag", str(s.ag == 2 ? "ground" : "airborne"));
    if (!s.zero_position) {
        kv("lat", num(s.lat));
        kv("lon", num(s.lon));
    }
    if (s.alt_ft)
        kv(s.alt_geometric ? "alt_geom" : "alt_baro", std::to_string(*s.alt_ft));
    if (s.aux_alt_ft)
        kv(s.alt_geometric ? "alt_baro" : "alt_geom", std::to_string(*s.aux_alt_ft));
    kv("nic", std::to_string(s.nic));
    if (s.ag == 0 || s.ag == 1) {
        if (s.ns_kt && s.ew_kt) {
            const double n = *s.ns_kt, e = *s.ew_kt;
            kv("gs", num(std::hypot(n, e)));
            if (n != 0.0 || e != 0.0) {
                double t = std::atan2(e, n) * 180.0 / 3.14159265358979323846;
                if (t < 0)
                    t += 360.0;
                kv("track", num(t));
            }
        }
        if (s.vrate_fpm) {
            kv("vrate", std::to_string(*s.vrate_fpm));
            kv("vrate_src", str(s.vrate_geometric ? "geometric" : "barometric"));
        }
    } else if (s.ag == 2) {
        if (s.ground_speed_kt)
            kv("gs", std::to_string(*s.ground_speed_kt));
        kv("track_type", std::to_string(s.track_type));
        if (s.track_type == 1)
            kv("track", num(s.track_deg));
        else if (s.track_type >= 2)
            kv("heading", num(s.track_deg));
    }
    if (s.type == 1 || s.type == 3) {
        kv("category", str(std::string(1, "ABCDE"[s.emitter / 8]) + std::to_string(s.emitter % 8)));
        static const char* const names[] = {"none", "general", "medical", "minimum_fuel",
                                            "no_comms", "unlawful_interference", "downed_aircraft"};
        if (s.emergency <= 6)
            kv("emergency", str(names[s.emergency]));
        kv("emergency_code", std::to_string(s.emergency));
        kv("nac_p", std::to_string(s.nacp));
        if (r.squawk_row) {
            kv("squawk", str(s.call.substr(0, 4)));
        } else if (s.csid) {
            std::string c = s.call;
            while (!c.empty() && c.back() == ' ')
                c.pop_back();
            if (!c.empty())
                kv("callsign", str(c));
        }
    }
    return o + "}";
}

}  // namespace

int main(int argc, char** argv)
{
    std::string expectPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
            expectPath = argv[++i];
        } else {
            std::cerr << "usage: uat_synth [--expect FILE]\n";
            return 2;
        }
    }
    buildMatrix();
    std::ofstream ex;
    if (!expectPath.empty()) {
        ex.open(expectPath);
        if (!ex) {
            std::cerr << "cannot write " << expectPath << "\n";
            return 1;
        }
    }
    for (const Row& r : g_rows) {
        const std::string hex = toHex(encodeUat(r.spec));
        std::cout << "-" << hex << ";\n";
        if (ex.is_open())
            ex << expectJson(r, hex) << "\n";
    }
    std::cerr << "uat_synth: " << g_rows.size() << " rows\n";
    return 0;
}
