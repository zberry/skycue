#define BOOST_TEST_MODULE runtime_config
#include <boost/test/unit_test.hpp>

#include "test_util.hpp"
#include "engine_test_support.hpp"

#include "conf_support.hpp"
#include "runtime_config.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <map>

using skycue_test::fileExists;

using namespace enginetest;
using skycued::FilterConfig;
using skycued::Overlay;
using skycued::RuntimeConfig;

namespace {

void writeFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}


bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// Captures what is written to stderr (the log) while it lives.
struct StderrCapture {
    std::string path;
    int saved = -1;
    explicit StderrCapture(const std::string& p) : path(p)
    {
        std::fflush(stderr);
        saved = ::dup(2);
        std::FILE* f = std::fopen(path.c_str(), "w");
        ::dup2(::fileno(f), 2);
        std::fclose(f);
    }
    ~StderrCapture() { restore(); }
    std::string text()
    {
        std::fflush(stderr);
        return readAll(path);
    }
    void restore()
    {
        if (saved < 0)
            return;
        std::fflush(stderr);
        ::dup2(saved, 2);
        ::close(saved);
        saved = -1;
    }
};

using ConfMap = std::map<std::string, std::string>;  // dotted key -> TOML literal

// This suite's skycue.toml: the receiver and track timeout every test uses, plus `extra`.
std::string headConf(const ConfMap& extra = {})
{
    ConfMap kv{{"receiver.lat", "38.9"}, {"receiver.lon", "-77.0"}, {"cues.track_timeout_s", "5"}};
    for (const auto& [k, v] : extra)
        kv[k] = v;
    return skycue_test::tomlConf(kv);
}

// A TOML string literal of a path without quotes or backslashes.
std::string str(const std::string& text) { return "\"" + text + "\""; }

// A conf file in a temp dir, an Engine built from it (recording the sent
// stream), and a RuntimeConfig on the Engine's io thread.
struct RtFx {
    TempDir tmp;
    std::string conf, rec;
    FakeSource* src = nullptr;
    std::unique_ptr<EngineFx> fx;
    std::unique_ptr<RuntimeConfig> rc;

    explicit RtFx(const std::string& conf_text, const std::map<std::string, std::string>& files = {})
    {
        conf = tmp.path + "/skycue.toml";
        rec = tmp.path + "/rec";
        ::mkdir(rec.c_str(), 0755);
        for (const auto& f : files)
            writeFile(tmp.path + "/" + f.first, f.second);
        writeFile(conf, conf_text);
    }
    void start(bool watch = false)
    {
        fx = std::make_unique<EngineFx>(skycued::parseConfigFile(conf), fakeSource(src), 10s, rec);
        fx->t.call([&] {
            rc = std::make_unique<RuntimeConfig>(fx->t.io, conf, *fx->engine);
            rc->load();
            if (watch)
                rc->startWatching();
        });
    }
    ~RtFx()
    {
        if (fx)
            fx->t.call([&] { rc.reset(); });
    }
    std::string path(const std::string& name) const { return tmp.path + "/" + name; }
    FilterConfig filters()
    {
        return fx->t.call([&] { return fx->engine->snapshot().filters; });
    }
    std::vector<std::string> allow()
    {
        return fx->t.call([&] { return fx->engine->allowList().entries(); });
    }
    std::vector<std::string> block()
    {
        return fx->t.call([&] { return fx->engine->blockList().entries(); });
    }
    void applyUi(const Overlay& d)
    {
        fx->t.call([&] { rc->applyUi(d, "ui"); });
    }
    std::string promote()
    {
        return fx->t.call([&] { return rc->promoteToBaseline(); });
    }
    Overlay overlay()
    {
        return fx->t.call([&] { return rc->overlay(); });
    }
    std::string sent()
    {
        fx->t.call([&] { fx->sent->flush(); });
        return streamText(rec, "sent");
    }
    template <class Pred> bool waitFor(Pred p, std::chrono::milliseconds timeout = 2500ms)
    {
        const auto deadline = steady::now() + timeout;
        while (steady::now() < deadline) {
            if (p())
                return true;
            std::this_thread::sleep_for(50ms);
        }
        return p();
    }
};

using Strings = std::vector<std::string>;

}  // namespace

// ---- RuntimeConfig ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(overlay_applied_on_load)
{
    RtFx f(headConf());
    writeFile(f.conf, headConf({{"filters.max_range_nm", "150"}, {"filters.allow_list_file", str(f.path("allow.txt"))}}));
    writeFile(f.path("allow.txt"), "a1b2c3\nb1b1b1\n");
    writeFile(f.path("skycue.runtime.json"),
              "{\"version\":1,\"zzz\":1,\"filters\":{\"max_range_nm\":42},"
              "\"lists\":{\"allow\":{\"add\":[\"N123AB\"],\"remove\":[\"B1B1B1\"]},\"block\":{\"add\":[\"c0ffee\"]}}}");
    f.start();
    BOOST_TEST(f.filters().max_range_nm == 42.0);
    BOOST_TEST(f.filters().min_elevation_deg == 0.0);
    BOOST_TEST(f.allow() == (Strings{"a1b2c3", "N123AB"}));
    BOOST_TEST(f.block() == (Strings{"c0ffee"}));
    Overlay o = f.overlay();
    BOOST_TEST(o.max_range_nm.value_or(0) == 42.0);
    BOOST_TEST(!o.min_elevation_deg);
    BOOST_TEST(o.allow_add == (Strings{"N123AB"}));
    BOOST_TEST(o.allow_remove == (Strings{"B1B1B1"}));
    BOOST_TEST(o.block_add == (Strings{"c0ffee"}));
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 42.0);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->overlayPath(); }) == f.path("skycue.runtime.json"));
    const std::string sent = f.sent();
    BOOST_TEST(has(sent, "startup: max_range_nm=42 min_elevation_deg=0"));
    BOOST_TEST(has(sent, "startup: allow_list=2 entries block_list=1 entries"));
}

BOOST_AUTO_TEST_CASE(malformed_or_missing_overlay_ignored_on_load)
{
    {
        RtFx f(headConf({{"filters.max_range_nm", "150"}}), {{"skycue.runtime.json", "{\"version\":1,"}});
        StderrCapture err(f.path("stderr.txt"));
        f.start();
        err.restore();
        BOOST_TEST(f.filters().max_range_nm == 150.0);
        BOOST_TEST(has(err.text(), "ERROR"));
        BOOST_TEST(has(err.text(), "skycue.runtime.json.rejected"));
        // Moved aside, so a later change from the UI cannot overwrite it.
        BOOST_TEST(!fileExists(f.path("skycue.runtime.json")));
        BOOST_TEST(readAll(f.path("skycue.runtime.json.rejected")) == "{\"version\":1,");
    }
    {
        // A bad entry or an out-of-range number makes the whole file malformed.
        RtFx f(headConf(),
               {{"skycue.runtime.json",
                 "{\"version\":1,\"filters\":{\"max_range_nm\":42},\"lists\":{\"allow\":{\"add\":[\"not ok!\"]}}}"}});
        f.start();
        BOOST_TEST(f.filters().max_range_nm == 150.0);
        BOOST_TEST(f.allow().empty());
    }
    {
        RtFx f(headConf({{"filters.min_elevation_deg", "2"}}));
        f.start();
        BOOST_TEST(f.filters().min_elevation_deg == 2.0);
        BOOST_TEST(!f.overlay().max_range_nm);
        BOOST_TEST(f.sent().find("startup:") == f.sent().rfind("startup:"));  // only the Engine's own record
    }
}

BOOST_AUTO_TEST_CASE(apply_ui_writes_overlay_and_engine)
{
    const std::string conf_text = headConf({{"filters.max_range_nm", "150"}});
    RtFx f(conf_text);
    f.start();
    Overlay d;
    d.min_elevation_deg = 5;
    d.block_add = {" c0ffee "};
    d.allow_add = {"N123AB"};
    f.applyUi(d);
    BOOST_TEST(f.filters().min_elevation_deg == 5.0);
    BOOST_TEST(f.filters().max_range_nm == 150.0);
    BOOST_TEST(f.block() == (Strings{"c0ffee"}));
    BOOST_TEST(f.allow() == (Strings{"N123AB"}));
    const std::string json = readAll(f.path("skycue.runtime.json"));
    BOOST_TEST(has(json, "\"version\": 1"));
    BOOST_TEST(has(json, "\"min_elevation_deg\": 5"));
    BOOST_TEST(!has(json, "max_range_nm"));
    BOOST_TEST(has(json, "\"c0ffee\""));
    BOOST_TEST(!fileExists(f.path("skycue.runtime.json.tmp")));
    BOOST_TEST(readAll(f.conf) == conf_text);  // only promote writes skycue.toml
    BOOST_TEST(has(f.sent(), "ui: max_range_nm=150 min_elevation_deg=5"));

    // A remove cancels the add (and vice versa): the overlay holds no delta.
    Overlay r;
    r.allow_remove = {"n123ab"};
    f.applyUi(r);
    BOOST_TEST(f.allow().empty());
    BOOST_TEST(f.overlay().allow_add.empty());
    BOOST_TEST(f.overlay().allow_remove.empty());

    // A bad entry or value throws and changes nothing.
    const std::string json_before = readAll(f.path("skycue.runtime.json"));
    Overlay bad;
    bad.block_add = {"zz"};
    bad.allow_add = {"not ok!"};
    BOOST_CHECK_THROW(f.applyUi(bad), std::invalid_argument);
    try {
        f.applyUi(bad);
    } catch (const std::invalid_argument& e) {
        BOOST_TEST(has(e.what(), "not ok!"));
    }
    Overlay bad2;
    bad2.max_range_nm = 0;
    BOOST_CHECK_THROW(f.applyUi(bad2), std::invalid_argument);
    BOOST_TEST(f.block() == (Strings{"c0ffee"}));
    BOOST_TEST(f.filters().max_range_nm == 150.0);
    BOOST_TEST(f.overlay().block_add == (Strings{"c0ffee"}));
    BOOST_TEST(readAll(f.path("skycue.runtime.json")) == json_before);

    // The file reads back as the same overlay.
    RtFx g(conf_text, {{"skycue.runtime.json", readAll(f.path("skycue.runtime.json"))}});
    g.start();
    BOOST_TEST(g.filters().min_elevation_deg == 5.0);
    BOOST_TEST(g.block() == (Strings{"c0ffee"}));
}

BOOST_AUTO_TEST_CASE(apply_ui_remove_of_file_entry_and_readd)
{
    RtFx f{headConf()};
    writeFile(f.path("allow.txt"), "a1b2c3\n");
    writeFile(f.conf, headConf({{"filters.allow_list_file", str(f.path("allow.txt"))}}));
    f.start();
    Overlay r;
    r.allow_remove = {"A1B2C3"};
    f.applyUi(r);
    BOOST_TEST(f.allow().empty());
    BOOST_TEST(f.overlay().allow_remove == (Strings{"A1B2C3"}));
    Overlay a;
    a.allow_add = {"a1b2c3"};
    f.applyUi(a);  // cancels the remove
    BOOST_TEST(f.allow() == (Strings{"a1b2c3"}));
    BOOST_TEST(f.overlay().allow_remove.empty());
    BOOST_TEST(f.overlay().allow_add.empty());  // already in the file
    BOOST_TEST(!fileExists(f.path("skycue.runtime.json")));  // the overlay is empty again
}

// The overrides file as save-as-baseline writes it: a header line, a blank
// line, then the [filters] table (checked against `body`).
bool isOverridesText(const std::string& text, const std::string& body)
{
    const std::string head = "# written by skycued ";
    const std::string tail = "; settings saved from the web UI; filter keys apply live, the rest on restart\n\n[filters]\n";
    const size_t nl = text.find('\n');
    if (text.compare(0, head.size(), head) != 0 || nl == std::string::npos)
        return false;
    const std::string first = text.substr(0, nl + 1);
    // "# written by skycued 2026-11-08T12:00:00Z; settings saved from the web UI; ..."
    const size_t stamp_end = head.size() + 20;
    return first.size() > stamp_end && first[head.size() + 19] == 'Z' && text.compare(stamp_end, std::string::npos, tail + body) == 0;
}

BOOST_AUTO_TEST_CASE(save_baseline_writes_overrides_toml)
{
    const std::string before = "# skycue configuration\n"
                               "[receiver]\n"
                               "lat = 38.9\n"
                               "lon = -77.0\n"
                               "\n"
                               "[filters]\n"
                               "# Ignore aircraft farther than this (nm).\n"
                               "max_range_nm = 150\n"
                               "#min_elevation_deg = 7\n"
                               "[recording]\n"
                               "formats = [\"jsonl\", \"csv\"]\n"
                               "[cues]\n"
                               "track_timeout_s = 5\n";
    RtFx f(before);
    ::chmod(f.conf.c_str(), 0640);
    f.start();
    const std::string overrides = f.path("skycue.overrides.toml");
    BOOST_TEST(f.fx->t.call([&] { return f.rc->overridesPath(); }) == overrides);
    Overlay d;
    d.max_range_nm = 42.5;
    d.min_elevation_deg = -1.25;
    f.applyUi(d);
    BOOST_TEST(readAll(f.conf) == before);
    BOOST_TEST(f.promote() == "");
    BOOST_TEST(readAll(f.conf) == before);  // byte for byte: skycued never writes the main file
    const std::string text = readAll(overrides);
    BOOST_TEST(isOverridesText(text, "max_range_nm = 42.5\nmin_elevation_deg = -1.25\n"), text);
    BOOST_TEST(!fileExists(overrides + ".tmp"));
    BOOST_TEST(!fileExists(f.path("skycue.runtime.json")));
    BOOST_TEST(!f.overlay().max_range_nm);
    BOOST_TEST(f.filters().max_range_nm == 42.5);
    BOOST_TEST(f.filters().min_elevation_deg == -1.25);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 42.5);
    BOOST_TEST(!has(f.sent(), "save-baseline:"));  // nothing effective changed
    BOOST_TEST(skycued::parseConfigFile(f.conf).min_elevation_deg == -1.25);
    // Nothing to promote: the overrides file is not touched.
    struct stat st2{};
    ::stat(overrides.c_str(), &st2);
    BOOST_TEST(f.promote() == "");
    struct stat st3{};
    ::stat(overrides.c_str(), &st3);
    BOOST_TEST(st2.st_ino == st3.st_ino);
    BOOST_TEST(st2.st_mtime == st3.st_mtime);
}

BOOST_AUTO_TEST_CASE(write_overrides_groups_tables_and_keeps_header)
{
    RtFx f("[receiver]\nlat = 38.9\nlon = -77.0\n");
    f.start();
    const std::string overrides = f.path("skycue.overrides.toml");
    // an edit made by hand: another table, written before the filters
    writeFile(overrides, "[fisb]\nenable = false\n");
    Overlay d;
    d.max_range_nm = 42.5;
    d.min_elevation_deg = -1.25;
    f.applyUi(d);
    BOOST_TEST(f.promote() == "");
    const std::string text = readAll(overrides);
    BOOST_TEST(text.compare(0, 20, "# written by skycued") == 0, text);
    BOOST_TEST(has(text, "; settings saved from the web UI; filter keys apply live, the rest on restart\n"), text);
    BOOST_TEST(has(text, "\n[fisb]\nenable = false\n"), text);
    BOOST_TEST(has(text, "\n[filters]\nmax_range_nm = 42.5\nmin_elevation_deg = -1.25\n"), text);
    BOOST_TEST(text.find("[filters]") < text.find("[fisb]"), text);  // keySpecs() order
}

BOOST_AUTO_TEST_CASE(overrides_bootstrap_key_rejected_by_watcher)
{
    RtFx f("[receiver]\nlat = 38.9\nlon = -77.0\n");
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    const auto port = f.fx->t.call([&] { return f.rc->effective().web_port; });
    writeFile(f.path("skycue.overrides.toml"), "[web]\nport = 81\n");
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "skycue.overrides.toml:2:"); }, 4000ms));
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().web_port; }) == port);
}

BOOST_AUTO_TEST_CASE(promote_appends_to_list_files_and_clears_overlay)
{
    RtFx f{headConf()};
    const std::string allow_before = "# allow list\na1b2c3\nN123AB  # a friend\nb1b1b1\n";
    writeFile(f.path("allow.txt"), allow_before);
    writeFile(f.conf, headConf({{"filters.allow_list_file", str(f.path("allow.txt"))}, {"filters.block_list_file", str(f.path("block.txt"))}}));
    // block.txt is missing: an empty list until it is created.
    writeFile(f.path("block.txt"), "");
    f.start();
    ::unlink(f.path("block.txt").c_str());
    Overlay d;
    d.allow_add = {"c0ffee", "G-ABCD"};
    d.allow_remove = {"n123ab"};
    d.block_add = {"dead01"};
    f.applyUi(d);
    BOOST_TEST(f.allow() == (Strings{"a1b2c3", "b1b1b1", "c0ffee", "G-ABCD"}));
    const std::string conf_before = readAll(f.conf);
    BOOST_TEST(f.promote() == "");
    BOOST_TEST(readAll(f.path("allow.txt")) == "# allow list\n"
                                                 "a1b2c3\n"
                                                 "# removed by skycue save-baseline: N123AB  # a friend\n"
                                                 "b1b1b1\n"
                                                 "c0ffee\n"
                                                 "G-ABCD\n");
    BOOST_TEST(readAll(f.path("block.txt")) == "dead01\n");
    BOOST_TEST(readAll(f.conf) == conf_before);  // no filter override: not rewritten
    BOOST_TEST(!fileExists(f.path("skycue.runtime.json")));
    BOOST_TEST(f.overlay().allow_add.empty());
    BOOST_TEST(f.overlay().allow_remove.empty());
    BOOST_TEST(f.overlay().block_add.empty());
    BOOST_TEST(f.allow() == (Strings{"a1b2c3", "b1b1b1", "c0ffee", "G-ABCD"}));
    BOOST_TEST(f.block() == (Strings{"dead01"}));
    BOOST_TEST(!has(f.sent(), "save-baseline:"));
}

BOOST_AUTO_TEST_CASE(promote_without_list_file_keeps_delta)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}}));
    f.start();
    Overlay d;
    d.max_range_nm = 60;
    d.block_add = {"dead01"};
    f.applyUi(d);
    const std::string err = f.promote();
    BOOST_TEST(has(err, "filters.block_list_file"));
    BOOST_TEST(!has(err, "filters.allow_list_file"));
    BOOST_TEST(has(readAll(f.path("skycue.overrides.toml")), "max_range_nm = 60.0\n"));
    Overlay o = f.overlay();
    BOOST_TEST(!o.max_range_nm);
    BOOST_TEST(o.block_add == (Strings{"dead01"}));
    BOOST_TEST(has(readAll(f.path("skycue.runtime.json")), "dead01"));
    BOOST_TEST(!has(readAll(f.path("skycue.runtime.json")), "max_range_nm"));
    BOOST_TEST(f.block() == (Strings{"dead01"}));
    BOOST_TEST(f.filters().max_range_nm == 60.0);
}

BOOST_AUTO_TEST_CASE(watch_detects_conf_edit_within_2s)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}}));
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(f.conf, headConf({{"filters.max_range_nm", "99"}, {"web.port", "8081"}}));
    // Applied once the file has kept its stamp for two polls: about 2 s at most.
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 99.0; }, 2600ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "web.port"); }, 500ms));
    BOOST_TEST(has(err.text(), "requires restart"));
    // Back to the running value: no restart needed, no warning.
    writeFile(f.conf, headConf({{"filters.max_range_nm", "98"}}));
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 98.0; }, 2600ms));
    err.restore();
    BOOST_TEST(countOf(err.text(), "requires restart") == 1u);
    BOOST_TEST(has(f.sent(), "file:skycue.toml: max_range_nm=99 min_elevation_deg=0"));
    // The web port keeps its running value; the filters take the new one.
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().web_port; }) == 8080);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 98.0);
}

BOOST_AUTO_TEST_CASE(record_raw_and_fisb_edits_need_restart)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}}));
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(f.conf, headConf({{"filters.max_range_nm", "97"}, {"recording.raw", "true"}, {"fisb.enable", "false"}, {"fisb.experimental", "true"}}));
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 97.0; }, 2600ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "fisb.experimental"); }, 500ms));
    err.restore();
    const std::string text = err.text();
    BOOST_TEST(has(text, "requires restart"), text);
    BOOST_TEST(has(text, "recording.raw"), text);
    BOOST_TEST(has(text, "fisb.enable"), text);
}

BOOST_AUTO_TEST_CASE(conf_edit_overridden_by_overlay_warns)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}, {"filters.min_elevation_deg", "0"}}));
    f.start(true);
    Overlay d;
    d.max_range_nm = 40;
    f.applyUi(d);
    StderrCapture err(f.path("stderr.txt"));
    // Both edited; only filters.max_range_nm is in the overlay.
    writeFile(f.conf, headConf({{"filters.max_range_nm", "99"}, {"filters.min_elevation_deg", "2"}}));
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 2.0; }, 2600ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "filters.max_range_nm edited"); }, 500ms));
    // The overlay keeps winning: the behaviour is unchanged.
    BOOST_TEST(f.filters().max_range_nm == 40.0);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 40.0);
    // Another edit of a key the overlay does not hold: no warning for it.
    writeFile(f.conf, headConf({{"filters.max_range_nm", "99"}, {"filters.min_elevation_deg", "3"}}));
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 3.0; }, 2600ms));
    err.restore();
    const std::string text = err.text();
    BOOST_TEST(countOf(text, "WARN  runtime: filters.max_range_nm edited in skycue.toml but overridden by "
                             "skycue.runtime.json (use the UI or \"save as baseline\")") == 1u);
    BOOST_TEST(!has(text, "filters.min_elevation_deg edited"));
}

BOOST_AUTO_TEST_CASE(watch_ignores_own_writes)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}}));
    writeFile(f.path("allow.txt"), "a1b2c3\n");
    writeFile(f.conf, headConf({{"filters.max_range_nm", "150"}, {"filters.allow_list_file", str(f.path("allow.txt"))}}));
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    Overlay d;
    d.max_range_nm = 70;
    d.allow_add = {"c0ffee"};
    f.applyUi(d);
    std::this_thread::sleep_for(1300ms);
    BOOST_TEST(f.promote() == "");  // writes skycue.overrides.toml and allow.txt, deletes the overlay
    std::this_thread::sleep_for(1300ms);
    err.restore();
    BOOST_TEST(!has(err.text(), "changed on disk"));
    BOOST_TEST(has(readAll(f.path("allow.txt")), "c0ffee"));
    const std::string sent = f.sent();
    BOOST_TEST(has(sent, "ui: max_range_nm=70"));
    BOOST_TEST(!has(sent, "file:"));
    BOOST_TEST(f.filters().max_range_nm == 70.0);
}

BOOST_AUTO_TEST_CASE(watch_rejects_malformed_edit_keeps_previous)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}}));
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(f.conf, headConf({{"filters.max_range_nm", "\"abc\""}}));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "ERROR"); }, 2600ms));
    std::this_thread::sleep_for(1100ms);  // more polls: still logged once
    BOOST_TEST(countOf(err.text(), "ERROR") == 1u);
    BOOST_TEST(has(err.text(), "filters.max_range_nm"));
    BOOST_TEST(f.filters().max_range_nm == 150.0);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 150.0);
    writeFile(f.conf, headConf({{"filters.max_range_nm", "77"}}));
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 77.0; }, 2600ms));
    err.restore();
}

BOOST_AUTO_TEST_CASE(watch_detects_list_file_edit)
{
    RtFx f{headConf()};
    writeFile(f.path("block.txt"), "a1b2c3\n");
    writeFile(f.conf, headConf({{"filters.block_list_file", str(f.path("block.txt"))}}));
    f.start(true);
    BOOST_TEST(f.block() == (Strings{"a1b2c3"}));
    writeFile(f.path("block.txt"), "a1b2c3\nN99ZZ\n");
    BOOST_TEST(f.waitFor([&] { return f.block() == (Strings{"a1b2c3", "N99ZZ"}); }, 2600ms));
    BOOST_TEST(has(f.sent(), "file:block.txt: "));
    // A hand edit of the overlay applies too.
    writeFile(f.path("skycue.runtime.json"), "{\"version\":1,\"filters\":{\"min_elevation_deg\":3}}\n");
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 3.0; }, 2600ms));
    BOOST_TEST(has(f.sent(), "file:skycue.runtime.json: "));
}

BOOST_AUTO_TEST_CASE(list_file_relative_to_state_dir)
{
    RtFx f{headConf()};
    const std::string state = f.path("state");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    writeFile(state + "/allow.list", "a1b2c3\n");
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.allow_list_file", str("allow.list")}}));
    f.start();
    BOOST_TEST(f.allow() == (Strings{"a1b2c3"}));
    // The overlay lives in paths.state_dir too, not next to the config.
    BOOST_TEST(f.fx->t.call([&] { return f.rc->overlayPath(); }) == state + "/skycue.runtime.json");
    Overlay d;
    d.allow_add = {"N123AB"};
    f.applyUi(d);
    BOOST_TEST(f.allow() == (Strings{"a1b2c3", "N123AB"}));
    BOOST_TEST(has(readAll(state + "/skycue.runtime.json"), "N123AB"));
    BOOST_TEST(!fileExists(f.path("skycue.runtime.json")));
    // Save as baseline appends to the list file in paths.state_dir.
    BOOST_TEST(f.promote() == "");
    BOOST_TEST(has(readAll(state + "/allow.list"), "N123AB"));
    BOOST_TEST(!fileExists(f.path("allow.list")));
}

BOOST_AUTO_TEST_CASE(list_reload_resolves_against_the_running_state_dir)
{
    // A live reload of a changed list file name resolves it against paths.state_dir as
    // at startup: a changed paths.state_dir takes effect only on a restart.
    RtFx f{headConf()};
    const std::string state = f.path("state"), moved = f.path("moved");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    BOOST_REQUIRE(::mkdir(moved.c_str(), 0755) == 0);
    writeFile(state + "/allow.list", "a1b2c3\n");
    writeFile(state + "/other.list", "beef01\n");
    writeFile(moved + "/other.list", "dead01\n");
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.allow_list_file", str("allow.list")}}));
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    BOOST_TEST(f.allow() == (Strings{"a1b2c3"}));
    writeFile(f.conf, headConf({{"paths.state_dir", str(moved)}, {"filters.allow_list_file", str("other.list")}}));
    BOOST_TEST(f.waitFor([&] { return f.allow() == (Strings{"beef01"}); }, 2600ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "paths.state_dir differ"); }, 500ms));
    // The new list file is watched at its resolved path.
    writeFile(state + "/other.list", "beef01\nN99ZZ\n");
    BOOST_TEST(f.waitFor([&] { return f.allow() == (Strings{"beef01", "N99ZZ"}); }, 2600ms));
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().allow_list_path; }) == state + "/other.list");
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().state_dir; }) == state);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->overlayPath(); }) == state + "/skycue.runtime.json");
}

BOOST_AUTO_TEST_CASE(watch_debounces_partial_write)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}}));
    f.start(true);
    // An editor writing the file in two parts: the first part alone parses
    // (max_range_nm = 1) but must not be applied. The parts are 0.6 s apart
    // around the first poll (1 s after start), so exactly one poll sees the
    // first part: without the debounce it would be applied.
    std::this_thread::sleep_for(700ms);
    writeFile(f.conf, headConf() + "[filters]\nmax_range_nm = 1");
    std::this_thread::sleep_for(600ms);
    {
        std::ofstream out(f.conf, std::ios::binary | std::ios::app);
        out << "23\n";
    }
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 123.0; }, 2600ms));
    std::this_thread::sleep_for(300ms);
    const std::string sent = f.sent();
    BOOST_TEST(countOf(sent, "file:skycue.toml:") == 1u);
    BOOST_TEST(!has(sent, "file:skycue.toml: max_range_nm=1 "));
}

BOOST_AUTO_TEST_CASE(atomic_write_preserves_symlink)
{
    RtFx f{headConf()};
    const std::string real_dir = f.path("real");
    ::mkdir(real_dir.c_str(), 0755);
    const std::string real_conf = real_dir + "/skycue.toml", real_allow = real_dir + "/allow.txt",
                      real_overrides = real_dir + "/skycue.overrides.toml";
    writeFile(real_allow, "a1b2c3\n");
    writeFile(real_conf, headConf({{"filters.max_range_nm", "150"}, {"filters.allow_list_file", str(f.path("allow.txt"))}}));
    ::unlink(f.conf.c_str());
    BOOST_REQUIRE(::symlink(real_conf.c_str(), f.conf.c_str()) == 0);
    BOOST_REQUIRE(::symlink("real/allow.txt", f.path("allow.txt").c_str()) == 0);  // relative target
    writeFile(real_overrides, "[filters]\nmin_elevation_deg = 1\n");
    BOOST_REQUIRE(::symlink("real/skycue.overrides.toml", f.path("skycue.overrides.toml").c_str()) == 0);
    f.start();
    Overlay d;
    d.max_range_nm = 55;
    d.allow_add = {"c0ffee"};
    f.applyUi(d);
    BOOST_TEST(f.promote() == "");
    struct stat st{};
    BOOST_TEST(::lstat(f.conf.c_str(), &st) == 0);
    BOOST_TEST(S_ISLNK(st.st_mode));
    BOOST_TEST(::lstat(f.path("allow.txt").c_str(), &st) == 0);
    BOOST_TEST(S_ISLNK(st.st_mode));
    BOOST_TEST(::lstat(f.path("skycue.overrides.toml").c_str(), &st) == 0);
    BOOST_TEST(S_ISLNK(st.st_mode));
    BOOST_TEST(has(readAll(real_overrides), "max_range_nm = 55.0\n"));
    BOOST_TEST(has(readAll(real_overrides), "min_elevation_deg = 1\n"));  // the keys it held stay
    BOOST_TEST(!has(readAll(real_conf), "55"));
    BOOST_TEST(readAll(real_allow) == "a1b2c3\nc0ffee\n");
    BOOST_TEST(!fileExists(real_overrides + ".tmp"));
    BOOST_TEST(!fileExists(f.path("skycue.overrides.toml.tmp")));
    BOOST_TEST(f.filters().max_range_nm == 55.0);
}

BOOST_AUTO_TEST_CASE(atomic_write_keeps_mode)
{
    RtFx f{headConf()};
    writeFile(f.path("allow.txt"), "a1b2c3\n");
    writeFile(f.conf, headConf({{"filters.allow_list_file", str(f.path("allow.txt"))}}));
    writeFile(f.path("skycue.overrides.toml"), "[filters]\nmax_range_nm = 100\n");
    ::chmod(f.path("skycue.overrides.toml").c_str(), 0640);
    ::chmod(f.path("allow.txt").c_str(), 0600);
    f.start();
    Overlay d;
    d.min_elevation_deg = 4;
    d.allow_add = {"c0ffee"};
    f.applyUi(d);
    BOOST_TEST(f.promote() == "");
    struct stat st{};
    ::stat(f.path("skycue.overrides.toml").c_str(), &st);
    BOOST_TEST((st.st_mode & 07777) == 0640u);
    ::stat(f.path("allow.txt").c_str(), &st);
    BOOST_TEST((st.st_mode & 07777) == 0600u);
    BOOST_TEST(has(readAll(f.path("skycue.overrides.toml")), "min_elevation_deg = 4.0\n"));
    // The owner is kept too (fchown, best effort); that needs root to test: manual.
}

BOOST_AUTO_TEST_CASE(restart_keys_cover_all_but_live_keys)
{
    const auto& restart = skycued::restartConfigKeys();
    const std::set<std::string> restart_set(restart.begin(), restart.end());
    BOOST_TEST(restart_set.size() == restart.size());  // no duplicates
    const std::set<std::string> live{"filters.max_range_nm", "filters.min_elevation_deg", "filters.allow_list_file",
                                     "filters.block_list_file"};
    for (const auto& k : live)
        BOOST_TEST(restart_set.count(k) == 0u, k);
    std::set<std::string> all = restart_set;
    all.insert(live.begin(), live.end());
    const auto& known = skycued::knownConfigKeys();
    BOOST_TEST((all == std::set<std::string>(known.begin(), known.end())));
    BOOST_TEST(restart_set.count("inputs.file") == 1u);
    BOOST_TEST(restart_set.count("paths.locations_file") == 1u);
    for (const char* k : {"stats.file", "stats.aircraft_max", "stats.save_s"})
        BOOST_TEST(restart_set.count(k) == 1u, k);
    for (const char* k : {"paths.state_dir", "paths.map_dir", "inputs.readsb.host", "inputs.dump978.host",
                          "inputs.dump978.enable", "inputs.readsb.enable"})
        BOOST_TEST(restart_set.count(k) == 1u, k);

    // b differs from a in every field but the live four.
    skycued::Config a;
    a.receiver_lat = 38.9;  // not NaN: NaN never equals itself
    a.receiver_lon = -77.0;
    skycued::Config b = a;
    b.receiver_lat = 1;
    b.receiver_lon = 2;
    b.receiver_alt_m = 3;
    b.antenna_height_m = 4;
    b.dump978_raw_port = 1;
    b.dump978_json_port = 2;
    b.readsb_json_port = 3;
    b.readsb_beast_port = 31005;
    b.track_decoder = "skycued";
    b.web_port = 4;
    b.cue_tcp_port = 5;
    b.cue_tcp_connect_targets = {{"h", 1}};
    b.cue_udp_targets = {{"h", 2}};
    b.record_dir = "r";
    b.record_formats = {"csv"};
    b.record_raw = true;
    b.min_interval_s = 2;
    b.max_interval_s = 20;
    b.track_timeout_s = 30;
    b.keyframe_interval_s = 5;
    b.web_bind = "0.0.0.0";
    b.record_max_gb = 1;
    b.trail_seconds = 60;
    b.web_allow_restart = true;
    b.fisb_enable = false;
    b.fisb_experimental = true;
    b.inputs_file = "i.json";
    b.locations_file = "l.csv";
    b.stats_file = "s.json";
    b.stats_aircraft_max = 100;
    b.stats_save_s = 10;
    b.sdr_1090_serial = "s1";
    b.sdr_978_serial = "s2";
    b.gain_1090 = "40";
    b.gain_978 = "41";
    b.state_dir = "/s";
    b.map_dir = "/m";
    b.readsb_host = "h1";
    b.dump978_host = "h2";
    b.dump978_enable = false;
    b.inputs_readsb_enable = false;
    const auto changed = skycued::restartKeysChanged(a, b);
    BOOST_TEST((std::set<std::string>(changed.begin(), changed.end()) == restart_set));
    BOOST_TEST(changed.size() == restart.size());
    // every restart key has a differs entry: nothing in restart_set is missing from `changed`
    for (const auto& k : restart)
        BOOST_TEST(std::count(changed.begin(), changed.end(), k) == 1, k + " has no differs entry");

    // The live four alone need no restart.
    skycued::Config c = a;
    c.max_range_nm = 10;
    c.min_elevation_deg = 5;
    c.allow_list_file = "a.txt";
    c.block_list_file = "b.txt";
    BOOST_TEST(skycued::restartKeysChanged(a, c).empty());
}

// ---- the overrides file (skycue.overrides.toml) ------------------------

BOOST_AUTO_TEST_CASE(promote_with_state_dir_leaves_conf_untouched)
{
    RtFx f{headConf()};
    const std::string state = f.path("state");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    writeFile(state + "/block.list", "");
    const std::string conf_text =
        headConf({{"paths.state_dir", str(state)}, {"filters.max_range_nm", "150"}, {"filters.block_list_file", str("block.list")}});
    writeFile(f.conf, conf_text);
    f.start();
    BOOST_TEST(f.fx->t.call([&] { return f.rc->overridesPath(); }) == state + "/skycue.overrides.toml");
    Overlay d;
    d.max_range_nm = 90;
    d.block_add = {"N123AB"};
    f.applyUi(d);
    BOOST_TEST(f.promote() == "");
    BOOST_TEST(readAll(f.conf) == conf_text);  // byte for byte
    BOOST_TEST(isOverridesText(readAll(state + "/skycue.overrides.toml"), "max_range_nm = 90.0\n"));
    BOOST_TEST(has(readAll(state + "/block.list"), "N123AB"));
    BOOST_TEST(!fileExists(state + "/skycue.runtime.json"));
    BOOST_TEST(!fileExists(f.path("skycue.overrides.toml")));
    BOOST_TEST(skycued::parseConfigFile(f.conf).max_range_nm == 90);
    BOOST_TEST(f.overlay().empty());
    BOOST_TEST(f.filters().max_range_nm == 90.0);
    BOOST_TEST(f.block() == (Strings{"N123AB"}));
    BOOST_TEST(!has(f.sent(), "save-baseline:"));  // nothing effective changed

    // A second save keeps the first one's value and adds the new one.
    Overlay e;
    e.min_elevation_deg = 2.5;
    f.applyUi(e);
    BOOST_TEST(f.promote() == "");
    const std::string overrides = readAll(state + "/skycue.overrides.toml");
    BOOST_TEST(isOverridesText(overrides, "max_range_nm = 90.0\nmin_elevation_deg = 2.5\n"), overrides);
    BOOST_TEST(readAll(f.conf) == conf_text);
    const skycued::Config c = skycued::parseConfigFile(f.conf);
    BOOST_TEST(c.min_elevation_deg == 2.5);
    BOOST_TEST(c.override_keys == (std::set<std::string>{"filters.max_range_nm", "filters.min_elevation_deg"}));
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().override_keys.count("filters.min_elevation_deg"); }) == 1u);
}

BOOST_AUTO_TEST_CASE(promote_with_state_dir_names_the_overrides_file_it_could_not_write)
{
    RtFx f{headConf()};
    const std::string state = f.path("state");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}}));
    f.start();
    Overlay d;
    d.max_range_nm = 90;
    f.applyUi(d);
    // A directory where the file should be: it cannot be read or replaced.
    BOOST_REQUIRE(::mkdir((state + "/skycue.overrides.toml").c_str(), 0755) == 0);
    const std::string err = f.promote();
    BOOST_TEST(err.rfind("skycue.overrides.toml not updated: ", 0) == 0u, err);
    BOOST_TEST(has(err, state + "/skycue.overrides.toml"), err);
    BOOST_TEST(has(err, "paths.state_dir"), err);
    BOOST_TEST(f.overlay().max_range_nm.value_or(0) == 90.0);  // stays in the overlay
    BOOST_TEST(f.filters().max_range_nm == 90.0);
}

BOOST_AUTO_TEST_CASE(watch_applies_overrides_edit_live)
{
    RtFx f{headConf()};
    const std::string state = f.path("state");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.min_elevation_deg", "1"}}));
    f.start(true);
    BOOST_TEST(f.filters().min_elevation_deg == 1.0);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(state + "/skycue.overrides.toml", "[filters]\nmin_elevation_deg = 3\n");
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 3.0; }, 4000ms));
    BOOST_TEST(has(f.sent(), "file:skycue.overrides.toml: "));
    // A key the file may not hold: logged, and the previous values stay.
    writeFile(state + "/skycue.overrides.toml", "[filters]\nmin_elevation_deg = 4\n[web]\nport = 81\n");
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "web.port cannot be set in skycue.overrides.toml"); }, 4000ms));
    BOOST_TEST(f.filters().min_elevation_deg == 3.0);
    // Deleted: skycue.toml's value applies again.
    ::unlink((state + "/skycue.overrides.toml").c_str());
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 1.0; }, 4000ms));
}

BOOST_AUTO_TEST_CASE(conf_valid_only_with_its_overrides_survives_a_restart)
{
    RtFx f{headConf()};
    const std::string state = f.path("state");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.max_range_nm", "150"}}));
    f.start(true);
    Overlay d;
    d.max_range_nm = 90;
    f.applyUi(d);
    BOOST_REQUIRE(f.promote() == "");
    StderrCapture err(f.path("stderr.txt"));
    // An edit that is valid only with the overrides file: accepted live...
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.max_range_nm", "0"}}));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "filters.max_range_nm edited in skycue.toml"); }, 4000ms));
    BOOST_TEST(!has(err.text(), "keeping the previous configuration"));
    BOOST_TEST(f.filters().max_range_nm == 90.0);
    // ...and on a restart.
    const double restarted = f.fx->t.call([&] {
        RuntimeConfig again(f.fx->t.io, f.conf, *f.fx->engine);
        again.load();
        return again.effective().max_range_nm;
    });
    BOOST_TEST(restarted == 90.0);
    BOOST_TEST(skycued::parseConfigFile(f.conf).max_range_nm == 90);
}

BOOST_AUTO_TEST_CASE(conf_edit_masked_by_overrides_logs_info)
{
    RtFx f{headConf()};
    const std::string state = f.path("state");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    writeFile(state + "/skycue.overrides.toml", "[filters]\nmax_range_nm = 90\n");
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.max_range_nm", "150"}, {"filters.min_elevation_deg", "0"}}));
    f.start(true);
    BOOST_TEST(f.filters().max_range_nm == 90.0);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.max_range_nm", "120"}, {"filters.min_elevation_deg", "2"}}));
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 2.0; }, 4000ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "filters.max_range_nm edited in skycue.toml"); }, 500ms));
    BOOST_TEST(f.filters().max_range_nm == 90.0);  // the overrides file still wins
    // An edit of a key the overrides file does not set: no line for it.
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.max_range_nm", "120"}, {"filters.min_elevation_deg", "3"}}));
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 3.0; }, 4000ms));
    err.restore();
    const std::string text = err.text();
    BOOST_TEST(countOf(text, "INFO  runtime: filters.max_range_nm edited in skycue.toml but skycue.overrides.toml sets it "
                             "(90); that value stays") == 1u, text);
    BOOST_TEST(!has(text, "filters.min_elevation_deg edited"), text);
    BOOST_TEST(!has(text, "WARN"), text);
}

BOOST_AUTO_TEST_CASE(overrides_edit_overridden_by_overlay_names_the_overrides_file)
{
    RtFx f{headConf()};
    const std::string state = f.path("state");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}, {"filters.max_range_nm", "150"}}));
    f.start(true);
    Overlay d;
    d.max_range_nm = 40;
    f.applyUi(d);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(state + "/skycue.overrides.toml", "[filters]\nmax_range_nm = 99\n");
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "filters.max_range_nm edited in"); }, 4000ms));
    err.restore();
    const std::string text = err.text();
    BOOST_TEST(countOf(text, "WARN  runtime: filters.max_range_nm edited in skycue.overrides.toml but overridden by "
                             "skycue.runtime.json (use the UI or \"save as baseline\")") == 1u, text);
    BOOST_TEST(!has(text, "edited in skycue.toml"), text);
    BOOST_TEST(f.filters().max_range_nm == 40.0);
}

BOOST_AUTO_TEST_CASE(promote_without_state_dir_writes_the_overrides_file_beside_the_conf)
{
    const std::string conf_text = headConf({{"filters.max_range_nm", "150"}});
    RtFx f(conf_text);
    f.start();
    BOOST_TEST(f.fx->t.call([&] { return f.rc->overridesPath(); }) == f.path("skycue.overrides.toml"));
    Overlay d;
    d.max_range_nm = 90;
    f.applyUi(d);
    BOOST_TEST(f.promote() == "");
    BOOST_TEST(readAll(f.conf) == conf_text);  // the main file is never written
    BOOST_TEST(isOverridesText(readAll(f.path("skycue.overrides.toml")), "max_range_nm = 90.0\n"));
    BOOST_TEST(!fileExists(f.path("skycue.runtime.json")));
}

BOOST_AUTO_TEST_CASE(save_baseline_readonly_dir_names_state_dir)
{
    if (::geteuid() == 0) {
        BOOST_TEST_MESSAGE("skipped: running as root, which writes to a read-only directory");
        return;
    }
    RtFx f(headConf({{"filters.max_range_nm", "150"}}));
    f.start();
    Overlay d;
    d.max_range_nm = 90;
    f.applyUi(d);
    const std::string before = readAll(f.conf);
    // A directory skycued cannot write, like the packages' /etc/skycue for a hand install.
    // The overlay (skycue.runtime.json) is in the same directory and was written
    // before, so only the new file is refused.
    ::chmod(f.tmp.path.c_str(), 0555);
    const std::string err = f.promote();
    ::chmod(f.tmp.path.c_str(), 0755);
    BOOST_TEST(err.rfind("skycue.overrides.toml not updated: ", 0) == 0u, err);
    BOOST_TEST(has(err, f.path("skycue.overrides.toml")), err);
    BOOST_TEST(has(err, "paths.state_dir"), err);
    BOOST_TEST(readAll(f.conf) == before);  // the main file is never written
    BOOST_TEST(!fileExists(f.path("skycue.overrides.toml")));
    BOOST_TEST(f.overlay().max_range_nm.value_or(0) == 90.0);  // stays in the overlay
    BOOST_TEST(f.filters().max_range_nm == 90.0);
}

BOOST_AUTO_TEST_CASE(runtime_overlay_does_not_override_pin)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}, {"filters.min_elevation_deg", "1"}}));
    // An overlay left on disk that sets both keys; --set pins the range only.
    writeFile(f.path("skycue.runtime.json"),
              "{\"version\":1,\"filters\":{\"max_range_nm\":30,\"min_elevation_deg\":6}}\n");
    f.fx = std::make_unique<EngineFx>(skycued::parseConfigFile(f.conf, "", {"filters.max_range_nm=88"}), fakeSource(f.src),
                                      10s, f.rec);
    f.fx->t.call([&] {
        f.rc = std::make_unique<RuntimeConfig>(f.fx->t.io, f.conf, *f.fx->engine);
        f.rc->setLayers({"filters.max_range_nm=88"}, {});
        f.rc->load();
    });
    BOOST_TEST(f.filters().max_range_nm == 88.0);    // the pin, not the overlay's 30
    BOOST_TEST(f.filters().min_elevation_deg == 6.0);  // not pinned: the overlay applies
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 88.0);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->pinnedBy("filters.max_range_nm"); }) == "set");
    BOOST_TEST(f.fx->t.call([&] { return f.rc->pinnedBy("filters.min_elevation_deg"); }) == "");
    Overlay d;
    d.max_range_nm = 20;
    BOOST_CHECK_THROW(f.applyUi(d), skycued::PinnedKeyError);
    Overlay c;
    c.clear_max_range_nm = true;
    BOOST_CHECK_THROW(f.applyUi(c), skycued::PinnedKeyError);
    BOOST_TEST(f.filters().max_range_nm == 88.0);
}

BOOST_AUTO_TEST_CASE(promote_skips_pinned_keys)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}, {"filters.min_elevation_deg", "1"}}));
    writeFile(f.path("skycue.runtime.json"),
              "{\"version\":1,\"filters\":{\"max_range_nm\":30,\"min_elevation_deg\":6}}\n");
    f.fx = std::make_unique<EngineFx>(skycued::parseConfigFile(f.conf, "", {"filters.max_range_nm=88"}), fakeSource(f.src),
                                      10s, f.rec);
    f.fx->t.call([&] {
        f.rc = std::make_unique<RuntimeConfig>(f.fx->t.io, f.conf, *f.fx->engine);
        f.rc->setLayers({"filters.max_range_nm=88"}, {});
        f.rc->load();
    });
    BOOST_TEST(f.promote() == "");
    const auto skipped = f.fx->t.call([&] { return f.rc->skippedPinned(); });
    BOOST_REQUIRE(skipped.size() == 1u);
    BOOST_TEST(skipped[0].first == "filters.max_range_nm");
    BOOST_TEST(skipped[0].second == "set");
    const std::string overrides = readAll(f.path("skycue.overrides.toml"));
    BOOST_TEST(!has(overrides, "max_range_nm"), overrides);
    BOOST_TEST(has(overrides, "min_elevation_deg = 6.0"), overrides);
    BOOST_TEST(f.overlay().empty());
    BOOST_TEST(!fileExists(f.path("skycue.runtime.json")));
    BOOST_TEST(f.filters().max_range_nm == 88.0);
}

BOOST_AUTO_TEST_CASE(watcher_warns_pinned_by_set)
{
    RtFx f(headConf({{"filters.max_range_nm", "150"}, {"filters.min_elevation_deg", "1"}}));
    f.fx = std::make_unique<EngineFx>(skycued::parseConfigFile(f.conf, "", {"filters.max_range_nm=88"}), fakeSource(f.src),
                                      10s, f.rec);
    f.fx->t.call([&] {
        f.rc = std::make_unique<RuntimeConfig>(f.fx->t.io, f.conf, *f.fx->engine);
        f.rc->setLayers({"filters.max_range_nm=88"}, {{"SKYCUE_FILTERS_MIN_ELEVATION_DEG", "2"}});
        f.rc->load();
        f.rc->startWatching();
    });
    BOOST_TEST(f.filters().max_range_nm == 88.0);
    BOOST_TEST(f.filters().min_elevation_deg == 2.0);
    StderrCapture err(f.path("stderr.txt"));
    // Both pinned keys edited in the file, and a free one: only the free one applies.
    writeFile(f.conf, headConf({{"filters.max_range_nm", "99"}, {"filters.min_elevation_deg", "5"},
                                {"filters.block_list_file", str(f.path("block.txt"))}}));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "pinned by --set"); }, 2600ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "pinned by SKYCUE_FILTERS_MIN_ELEVATION_DEG"); }, 500ms));
    err.restore();
    const std::string text = err.text();
    BOOST_TEST(has(text, "WARN  runtime: filters.max_range_nm edited in skycue.toml but pinned by --set (88); that value stays"), text);
    BOOST_TEST(has(text, "WARN  runtime: filters.min_elevation_deg edited in skycue.toml but pinned by SKYCUE_FILTERS_MIN_ELEVATION_DEG (2); that value stays"), text);
    BOOST_TEST(f.filters().max_range_nm == 88.0);
    BOOST_TEST(f.filters().min_elevation_deg == 2.0);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 88.0);
    // An edit that leaves the pinned keys alone warns for neither.
    StderrCapture err2(f.path("stderr2.txt"));
    writeFile(f.conf, headConf({{"filters.max_range_nm", "99"}, {"filters.min_elevation_deg", "5"}, {"web.port", "8081"}}));
    BOOST_TEST(f.waitFor([&] { return has(err2.text(), "requires restart"); }, 2600ms));
    err2.restore();
    BOOST_TEST(!has(err2.text(), "pinned by"), err2.text());
}

BOOST_AUTO_TEST_CASE(overlay_left_next_to_the_config_warns_when_state_dir_is_set)
{
    RtFx f{headConf()};
    const std::string state = f.path("state");
    BOOST_REQUIRE(::mkdir(state.c_str(), 0755) == 0);
    writeFile(f.path("skycue.runtime.json"), "{\"version\": 1, \"filters\": {\"max_range_nm\": 33}}\n");
    writeFile(f.conf, headConf({{"paths.state_dir", str(state)}}));
    StderrCapture err(f.path("stderr.txt"));
    f.start();
    err.restore();
    const std::string log = err.text();
    BOOST_TEST(has(log, "WARN "), log);
    BOOST_TEST(has(log, f.path("skycue.runtime.json")), log);
    BOOST_TEST(has(log, state + "/skycue.runtime.json"), log);
    BOOST_TEST(f.filters().max_range_nm == 150.0);  // not read from the old place
    // With the overlay in paths.state_dir there is nothing to warn about.
    RtFx g{headConf()};
    BOOST_REQUIRE(::mkdir(g.path("state").c_str(), 0755) == 0);
    writeFile(g.path("skycue.runtime.json"), "{\"version\": 1}\n");
    writeFile(g.path("state/skycue.runtime.json"), "{\"version\": 1}\n");
    writeFile(g.conf, headConf({{"paths.state_dir", str(g.path("state"))}}));
    StderrCapture err2(g.path("stderr.txt"));
    g.start();
    err2.restore();
    BOOST_TEST(!has(err2.text(), "skycue.runtime.json"), err2.text());
}

BOOST_AUTO_TEST_CASE(apply_config_path_keys_take_only_relative_names)
{
    RtFx f(headConf({}));
    f.start();
    using skycued::PathKeyError;
    using skycued::ReadOnlyKeyError;
    using V = skycued::toml_lite::Value;
    f.fx->t.call([&] {
        for (const char* key : {"inputs.file", "stats.file", "recording.dir", "paths.map_dir", "paths.locations_file",
                                "filters.allow_list_file", "filters.block_list_file"}) {
            BOOST_CHECK_THROW(f.rc->applyConfig({{key, V(std::string("/abs/x"))}}, {}, "ui"), PathKeyError);
            BOOST_CHECK_THROW(f.rc->applyConfig({{key, V(std::string("../x"))}}, {}, "ui"), ReadOnlyKeyError);
            BOOST_CHECK_THROW(f.rc->applyConfig({{key, V(std::string("a/../b"))}}, {}, "ui"), PathKeyError);
        }
        BOOST_CHECK(skycued::isRelativeName(""));
        BOOST_CHECK(skycued::isRelativeName("a/b..c/..d"));
        BOOST_CHECK(!skycued::isRelativeName(".."));
        f.rc->applyConfig({{"recording.dir", V(std::string("recordings"))}}, {}, "ui");
        f.rc->applyConfig({{"recording.dir", V(std::string())}}, {}, "ui");
    });
}

BOOST_AUTO_TEST_CASE(flag_keys_are_pinned)
{
    RtFx f(headConf({}));
    f.start();
    using skycued::PinnedKeyError;
    using V = skycued::toml_lite::Value;
    f.fx->t.call([&] {
        BOOST_TEST(f.rc->pinnedBy("inputs.readsb.enable") == "");
        f.rc->setFlagKeys({"inputs.readsb.enable"});
        BOOST_TEST(f.rc->pinnedBy("inputs.readsb.enable") == "flag");
        BOOST_CHECK_THROW(f.rc->applyConfig({{"inputs.readsb.enable", V(true)}}, {}, "ui"), PinnedKeyError);
    });
}

BOOST_AUTO_TEST_CASE(apply_config_orders_its_checks_and_reports_changes)
{
    RtFx f(headConf({{"cues.max_interval_s", "10"}}));
    f.start();
    using skycued::ConfigApplyResult;
    using skycued::ReadOnlyKeyError;
    using V = skycued::toml_lite::Value;
    f.fx->t.call([&] {
        BOOST_CHECK_THROW(f.rc->applyConfig({{"nope.key", V(int64_t(1))}}, {}, "ui"), std::invalid_argument);
        BOOST_CHECK_THROW(f.rc->applyConfig({{"web.bind", V(std::string("0.0.0.0"))}}, {}, "ui"), ReadOnlyKeyError);
        BOOST_CHECK_THROW(f.rc->applyConfig({{"web.trail_seconds", V(int64_t(300))}}, {"web.trail_seconds"}, "ui"),
                          std::invalid_argument);
        try {
            f.rc->applyConfig({{"cues.min_interval_s", V(int64_t(12))}}, {}, "ui");
            BOOST_ERROR("no throw");
        } catch (const std::invalid_argument& e) {
            const std::string w = e.what();
            BOOST_TEST(w.find("cues.") == 0u, w);
            BOOST_TEST(w.find("cues.min_interval_s") != std::string::npos, w);
            BOOST_TEST(w.find("skycue") == std::string::npos, w);
        }
        const ConfigApplyResult r = f.rc->applyConfig({{"fisb.enable", V(false)}, {"filters.max_range_nm", V(int64_t(90))}},
                                                      {"web.trail_seconds"}, "ui");
        BOOST_TEST(r.applied.size() == 2u);
        BOOST_TEST(r.pending_restart == std::vector<std::string>{"fisb.enable"}, boost::test_tools::per_element());
        BOOST_TEST(r.external.empty());
    });
    BOOST_TEST(f.filters().max_range_nm == 90.0);
}
