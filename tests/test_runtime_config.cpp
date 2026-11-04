#define BOOST_TEST_MODULE runtime_config
#include <boost/test/unit_test.hpp>

#include "engine_test_support.hpp"

#include "conf_rewrite.hpp"
#include "runtime_config.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <map>

using namespace enginetest;
using skycued::FilterConfig;
using skycued::Overlay;
using skycued::RuntimeConfig;
using skycued::conf_rewrite::rewriteKeys;

namespace {

void writeFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool exists(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
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

const char* const kConfHead = "RECEIVER_LAT=38.9\nRECEIVER_LON=-77.0\nTRACK_TIMEOUT_S=5\n";

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
        conf = tmp.path + "/skycue.conf";
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

// ---- conf_rewrite --------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rewrite_replaces_values_in_place)
{
    const std::string in = "# head\nA=1\n# A=commented\n  # B=x\nB=\"two words\"\nC=3\n";
    BOOST_TEST(rewriteKeys(in, {{"A", "42.5"}, {"B", "x"}}) ==
               "# head\nA=42.5\n# A=commented\n  # B=x\nB=x\nC=3\n");
    BOOST_TEST(rewriteKeys(in, {}) == in);
}

BOOST_AUTO_TEST_CASE(rewrite_quotes_values_that_need_it)
{
    BOOST_TEST(rewriteKeys("A=1\n", {{"A", "two words"}}) == "A=\"two words\"\n");
    BOOST_TEST(rewriteKeys("A=1\n", {{"A", "say \"hi\""}}) == "A='say \"hi\"'\n");
    BOOST_TEST(rewriteKeys("A=1\n", {{"A", ""}}) == "A=\n");
    BOOST_TEST(rewriteKeys("A=1\n", {{"A", "a/b:c,d=e"}}) == "A=a/b:c,d=e\n");
    BOOST_CHECK_THROW(rewriteKeys("A=1\n", {{"A", "it's \"both\""}}), std::invalid_argument);
    BOOST_CHECK_THROW(rewriteKeys("A=1\n", {{"A", "line\nbreak"}}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(rewrite_appends_missing_keys)
{
    BOOST_TEST(rewriteKeys("A=1\n", {{"B", "2"}, {"C", "3"}}) ==
               "A=1\n# added by skycue save-baseline\nB=2\nC=3\n");
    BOOST_TEST(rewriteKeys("A=1", {{"A", "5"}, {"B", "2"}}) == "A=5\n# added by skycue save-baseline\nB=2\n");
    BOOST_TEST(rewriteKeys("", {{"B", "2"}}) == "# added by skycue save-baseline\nB=2\n");
}

BOOST_AUTO_TEST_CASE(rewrite_every_assignment_of_a_key)
{
    // The last assignment is the one that takes effect, so all are rewritten.
    BOOST_TEST(rewriteKeys("A=1\nAB=1\nA=2\n", {{"A", "9"}}) == "A=9\nAB=1\nA=9\n");
}

BOOST_AUTO_TEST_CASE(rewrite_preserves_crlf)
{
    BOOST_TEST(rewriteKeys("# x\r\nA=1\r\nC=3\r\n", {{"A", "7"}, {"B", "2"}}) ==
               "# x\r\nA=7\r\nC=3\r\n# added by skycue save-baseline\r\nB=2\r\n");
}

// ---- RuntimeConfig ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(overlay_applied_on_load)
{
    RtFx f(kConfHead);
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=150\nALLOW_LIST_FILE=" + f.path("allow.txt") + "\n");
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
        RtFx f(std::string(kConfHead) + "MAX_RANGE_NM=150\n", {{"skycue.runtime.json", "{\"version\":1,"}});
        StderrCapture err(f.path("stderr.txt"));
        f.start();
        err.restore();
        BOOST_TEST(f.filters().max_range_nm == 150.0);
        BOOST_TEST(has(err.text(), "ERROR"));
        BOOST_TEST(has(err.text(), "skycue.runtime.json.rejected"));
        // Moved aside, so a later change from the UI cannot overwrite it.
        BOOST_TEST(!exists(f.path("skycue.runtime.json")));
        BOOST_TEST(readAll(f.path("skycue.runtime.json.rejected")) == "{\"version\":1,");
    }
    {
        // A bad entry or an out-of-range number makes the whole file malformed.
        RtFx f(std::string(kConfHead),
               {{"skycue.runtime.json",
                 "{\"version\":1,\"filters\":{\"max_range_nm\":42},\"lists\":{\"allow\":{\"add\":[\"not ok!\"]}}}"}});
        f.start();
        BOOST_TEST(f.filters().max_range_nm == 150.0);
        BOOST_TEST(f.allow().empty());
    }
    {
        RtFx f(std::string(kConfHead) + "MIN_ELEVATION_DEG=2\n");
        f.start();
        BOOST_TEST(f.filters().min_elevation_deg == 2.0);
        BOOST_TEST(!f.overlay().max_range_nm);
        BOOST_TEST(f.sent().find("startup:") == f.sent().rfind("startup:"));  // only the Engine's own record
    }
}

BOOST_AUTO_TEST_CASE(apply_ui_writes_overlay_and_engine)
{
    const std::string conf_text = std::string(kConfHead) + "MAX_RANGE_NM=150\n";
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
    BOOST_TEST(!exists(f.path("skycue.runtime.json.tmp")));
    BOOST_TEST(readAll(f.conf) == conf_text);  // only promote writes skycue.conf
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
    RtFx f{std::string(kConfHead)};
    writeFile(f.path("allow.txt"), "a1b2c3\n");
    writeFile(f.conf, std::string(kConfHead) + "ALLOW_LIST_FILE=" + f.path("allow.txt") + "\n");
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
    BOOST_TEST(!exists(f.path("skycue.runtime.json")));  // the overlay is empty again
}

BOOST_AUTO_TEST_CASE(promote_rewrites_conf_preserving_comments)
{
    const std::string before = "# skycue configuration\n"
                               "RECEIVER_LAT=38.9\n"
                               "RECEIVER_LON=-77.0\n"
                               "\n"
                               "# Ignore aircraft farther than this (nm).\n"
                               "MAX_RANGE_NM=150\n"
                               "#MIN_ELEVATION_DEG=7\n"
                               "RECORD_FORMATS=\"jsonl csv\"\n"
                               "TRACK_TIMEOUT_S=5\n";
    const std::string expected = "# skycue configuration\n"
                                 "RECEIVER_LAT=38.9\n"
                                 "RECEIVER_LON=-77.0\n"
                                 "\n"
                                 "# Ignore aircraft farther than this (nm).\n"
                                 "MAX_RANGE_NM=42.5\n"
                                 "#MIN_ELEVATION_DEG=7\n"
                                 "RECORD_FORMATS=\"jsonl csv\"\n"
                                 "TRACK_TIMEOUT_S=5\n"
                                 "# added by skycue save-baseline\n"
                                 "MIN_ELEVATION_DEG=-1.25\n";
    RtFx f(before);
    ::chmod(f.conf.c_str(), 0640);
    f.start();
    Overlay d;
    d.max_range_nm = 42.5;
    d.min_elevation_deg = -1.25;
    f.applyUi(d);
    BOOST_TEST(readAll(f.conf) == before);
    BOOST_TEST(f.promote() == "");
    BOOST_TEST(readAll(f.conf) == expected);  // byte for byte
    struct stat st{};
    ::stat(f.conf.c_str(), &st);
    BOOST_TEST((st.st_mode & 07777) == 0640u);
    BOOST_TEST(!exists(f.conf + ".tmp"));
    BOOST_TEST(!exists(f.path("skycue.runtime.json")));
    BOOST_TEST(!f.overlay().max_range_nm);
    BOOST_TEST(f.filters().max_range_nm == 42.5);
    BOOST_TEST(f.filters().min_elevation_deg == -1.25);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 42.5);
    BOOST_TEST(!has(f.sent(), "save-baseline:"));  // nothing effective changed
    BOOST_TEST(skycued::parseConfigFile(f.conf).min_elevation_deg == -1.25);
    // Nothing to promote: skycue.conf is not touched.
    struct stat st2{};
    ::stat(f.conf.c_str(), &st2);
    BOOST_TEST(f.promote() == "");
    struct stat st3{};
    ::stat(f.conf.c_str(), &st3);
    BOOST_TEST(st2.st_ino == st3.st_ino);
}

BOOST_AUTO_TEST_CASE(promote_appends_to_list_files_and_clears_overlay)
{
    RtFx f{std::string(kConfHead)};
    const std::string allow_before = "# allow list\na1b2c3\nN123AB  # a friend\nb1b1b1\n";
    writeFile(f.path("allow.txt"), allow_before);
    writeFile(f.conf, std::string(kConfHead) + "ALLOW_LIST_FILE=" + f.path("allow.txt") +
                          "\nBLOCK_LIST_FILE=" + f.path("block.txt") + "\n");
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
    BOOST_TEST(!exists(f.path("skycue.runtime.json")));
    BOOST_TEST(f.overlay().allow_add.empty());
    BOOST_TEST(f.overlay().allow_remove.empty());
    BOOST_TEST(f.overlay().block_add.empty());
    BOOST_TEST(f.allow() == (Strings{"a1b2c3", "b1b1b1", "c0ffee", "G-ABCD"}));
    BOOST_TEST(f.block() == (Strings{"dead01"}));
    BOOST_TEST(!has(f.sent(), "save-baseline:"));
}

BOOST_AUTO_TEST_CASE(promote_without_list_file_keeps_delta)
{
    RtFx f(std::string(kConfHead) + "MAX_RANGE_NM=150\n");
    f.start();
    Overlay d;
    d.max_range_nm = 60;
    d.block_add = {"dead01"};
    f.applyUi(d);
    const std::string err = f.promote();
    BOOST_TEST(has(err, "BLOCK_LIST_FILE"));
    BOOST_TEST(!has(err, "ALLOW_LIST_FILE"));
    BOOST_TEST(has(readAll(f.conf), "MAX_RANGE_NM=60\n"));
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
    RtFx f(std::string(kConfHead) + "MAX_RANGE_NM=150\n");
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=99\nWEB_PORT=8081\n");
    // Applied once the file has kept its stamp for two polls: about 2 s at most.
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 99.0; }, 2600ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "WEB_PORT"); }, 500ms));
    BOOST_TEST(has(err.text(), "requires restart"));
    // Back to the running value: no restart needed, no warning.
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=98\n");
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 98.0; }, 2600ms));
    err.restore();
    BOOST_TEST(countOf(err.text(), "requires restart") == 1u);
    BOOST_TEST(has(f.sent(), "file:skycue.conf: max_range_nm=99 min_elevation_deg=0"));
    // The web port keeps its running value; the filters take the new one.
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().web_port; }) == 8080);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 98.0);
}

BOOST_AUTO_TEST_CASE(record_raw_and_fisb_edits_need_restart)
{
    RtFx f(std::string(kConfHead) + "MAX_RANGE_NM=150\n");
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(f.conf, std::string(kConfHead) +
                          "MAX_RANGE_NM=97\nRECORD_RAW=yes\nFISB_ENABLE=no\nFISB_EXPERIMENTAL=yes\n");
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 97.0; }, 2600ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "FISB_EXPERIMENTAL"); }, 500ms));
    err.restore();
    const std::string text = err.text();
    BOOST_TEST(has(text, "requires restart"), text);
    BOOST_TEST(has(text, "RECORD_RAW"), text);
    BOOST_TEST(has(text, "FISB_ENABLE"), text);
}

BOOST_AUTO_TEST_CASE(conf_edit_overridden_by_overlay_warns)
{
    RtFx f(std::string(kConfHead) + "MAX_RANGE_NM=150\nMIN_ELEVATION_DEG=0\n");
    f.start(true);
    Overlay d;
    d.max_range_nm = 40;
    f.applyUi(d);
    StderrCapture err(f.path("stderr.txt"));
    // Both edited; only MAX_RANGE_NM is in the overlay.
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=99\nMIN_ELEVATION_DEG=2\n");
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 2.0; }, 2600ms));
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "MAX_RANGE_NM edited"); }, 500ms));
    // The overlay keeps winning: the behaviour is unchanged.
    BOOST_TEST(f.filters().max_range_nm == 40.0);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 40.0);
    // Another edit of a key the overlay does not hold: no warning for it.
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=99\nMIN_ELEVATION_DEG=3\n");
    BOOST_TEST(f.waitFor([&] { return f.filters().min_elevation_deg == 3.0; }, 2600ms));
    err.restore();
    const std::string text = err.text();
    BOOST_TEST(countOf(text, "WARN  runtime: MAX_RANGE_NM edited in skycue.conf but overridden by "
                             "skycue.runtime.json (use the UI or \"save as baseline\")") == 1u);
    BOOST_TEST(!has(text, "MIN_ELEVATION_DEG edited"));
}

BOOST_AUTO_TEST_CASE(watch_ignores_own_writes)
{
    RtFx f(std::string(kConfHead) + "MAX_RANGE_NM=150\n");
    writeFile(f.path("allow.txt"), "a1b2c3\n");
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=150\nALLOW_LIST_FILE=" + f.path("allow.txt") + "\n");
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    Overlay d;
    d.max_range_nm = 70;
    d.allow_add = {"c0ffee"};
    f.applyUi(d);
    std::this_thread::sleep_for(1300ms);
    BOOST_TEST(f.promote() == "");  // writes skycue.conf and allow.txt, deletes the overlay
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
    RtFx f(std::string(kConfHead) + "MAX_RANGE_NM=150\n");
    f.start(true);
    StderrCapture err(f.path("stderr.txt"));
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=abc\n");
    BOOST_TEST(f.waitFor([&] { return has(err.text(), "ERROR"); }, 2600ms));
    std::this_thread::sleep_for(1100ms);  // more polls: still logged once
    BOOST_TEST(countOf(err.text(), "ERROR") == 1u);
    BOOST_TEST(has(err.text(), "MAX_RANGE_NM"));
    BOOST_TEST(f.filters().max_range_nm == 150.0);
    BOOST_TEST(f.fx->t.call([&] { return f.rc->effective().max_range_nm; }) == 150.0);
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=77\n");
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 77.0; }, 2600ms));
    err.restore();
}

BOOST_AUTO_TEST_CASE(watch_detects_list_file_edit)
{
    RtFx f{std::string(kConfHead)};
    writeFile(f.path("block.txt"), "a1b2c3\n");
    writeFile(f.conf, std::string(kConfHead) + "BLOCK_LIST_FILE=" + f.path("block.txt") + "\n");
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

BOOST_AUTO_TEST_CASE(watch_debounces_partial_write)
{
    RtFx f(std::string(kConfHead) + "MAX_RANGE_NM=150\n");
    f.start(true);
    // An editor writing the file in two parts: the first part alone parses
    // (MAX_RANGE_NM=1) but must not be applied. The parts are 0.6 s apart
    // around the first poll (1 s after start), so exactly one poll sees the
    // first part: without the debounce it would be applied.
    std::this_thread::sleep_for(700ms);
    writeFile(f.conf, std::string(kConfHead) + "MAX_RANGE_NM=1");
    std::this_thread::sleep_for(600ms);
    {
        std::ofstream out(f.conf, std::ios::binary | std::ios::app);
        out << "23\n";
    }
    BOOST_TEST(f.waitFor([&] { return f.filters().max_range_nm == 123.0; }, 2600ms));
    std::this_thread::sleep_for(300ms);
    const std::string sent = f.sent();
    BOOST_TEST(countOf(sent, "file:skycue.conf:") == 1u);
    BOOST_TEST(!has(sent, "file:skycue.conf: max_range_nm=1 "));
}

BOOST_AUTO_TEST_CASE(atomic_write_preserves_symlink)
{
    RtFx f{std::string(kConfHead)};
    const std::string real_dir = f.path("real");
    ::mkdir(real_dir.c_str(), 0755);
    const std::string real_conf = real_dir + "/skycue.conf", real_allow = real_dir + "/allow.txt";
    writeFile(real_allow, "a1b2c3\n");
    writeFile(real_conf, std::string(kConfHead) + "MAX_RANGE_NM=150\nALLOW_LIST_FILE=" + f.path("allow.txt") + "\n");
    ::unlink(f.conf.c_str());
    BOOST_REQUIRE(::symlink(real_conf.c_str(), f.conf.c_str()) == 0);
    BOOST_REQUIRE(::symlink("real/allow.txt", f.path("allow.txt").c_str()) == 0);  // relative target
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
    BOOST_TEST(has(readAll(real_conf), "MAX_RANGE_NM=55\n"));
    BOOST_TEST(readAll(real_allow) == "a1b2c3\nc0ffee\n");
    BOOST_TEST(!exists(real_conf + ".tmp"));
    BOOST_TEST(!exists(f.conf + ".tmp"));
    BOOST_TEST(f.filters().max_range_nm == 55.0);
}

BOOST_AUTO_TEST_CASE(atomic_write_keeps_mode)
{
    RtFx f{std::string(kConfHead)};
    writeFile(f.path("allow.txt"), "a1b2c3\n");
    writeFile(f.conf, std::string(kConfHead) + "ALLOW_LIST_FILE=" + f.path("allow.txt") + "\n");
    ::chmod(f.conf.c_str(), 0640);
    ::chmod(f.path("allow.txt").c_str(), 0600);
    f.start();
    Overlay d;
    d.min_elevation_deg = 4;
    d.allow_add = {"c0ffee"};
    f.applyUi(d);
    BOOST_TEST(f.promote() == "");
    struct stat st{};
    ::stat(f.conf.c_str(), &st);
    BOOST_TEST((st.st_mode & 07777) == 0640u);
    ::stat(f.path("allow.txt").c_str(), &st);
    BOOST_TEST((st.st_mode & 07777) == 0600u);
    BOOST_TEST(has(readAll(f.conf), "MIN_ELEVATION_DEG=4\n"));
    // The owner is kept too (fchown, best effort); that needs root to test: manual.
}

BOOST_AUTO_TEST_CASE(restart_keys_cover_all_but_live_keys)
{
    const auto& restart = skycued::restartConfigKeys();
    const std::set<std::string> restart_set(restart.begin(), restart.end());
    BOOST_TEST(restart_set.size() == restart.size());  // no duplicates
    const std::set<std::string> live{"MAX_RANGE_NM", "MIN_ELEVATION_DEG", "ALLOW_LIST_FILE", "BLOCK_LIST_FILE"};
    for (const auto& k : live)
        BOOST_TEST(restart_set.count(k) == 0u, k);
    std::set<std::string> all = restart_set;
    all.insert(live.begin(), live.end());
    const auto& known = skycued::knownConfigKeys();
    BOOST_TEST((all == std::set<std::string>(known.begin(), known.end())));
    BOOST_TEST(restart_set.count("INPUTS_FILE") == 1u);
    BOOST_TEST(restart_set.count("LOCATIONS_FILE") == 1u);
    for (const char* k : {"STATS_FILE", "STATS_AIRCRAFT_MAX", "STATS_SAVE_S"})
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
    const auto changed = skycued::restartKeysChanged(a, b);
    BOOST_TEST((std::set<std::string>(changed.begin(), changed.end()) == restart_set));
    BOOST_TEST(changed.size() == restart.size());

    // The live four alone need no restart.
    skycued::Config c = a;
    c.max_range_nm = 10;
    c.min_elevation_deg = 5;
    c.allow_list_file = "a.txt";
    c.block_list_file = "b.txt";
    BOOST_TEST(skycued::restartKeysChanged(a, c).empty());
}
