#define BOOST_TEST_MODULE skycued_config_keys
#include <boost/test/unit_test.hpp>

#include "conf_support.hpp"
#include "config.hpp"
#include "conf_convert.hpp"
#include "json_lite.hpp"
#include "runtime_config.hpp"
#include "toml_lite.hpp"
#include "web_config.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace skycued;

namespace {

std::string slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(in, "cannot open " << path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Keys the example config may carry no comment for. Empty today: every key has one.
// A key added here is a recorded gap, not a design.
const std::set<std::string> kNoComment{};

// Keys sampleLiterals cannot make two valid distinct values for.
const std::set<std::string> kNoSample{};

// Two distinct TOML literals valid for the key, or empty strings.
std::pair<std::string, std::string> sampleLiterals(const KeySpec& s)
{
    // Keys whose text must have a particular shape.
    static const std::map<std::string, std::pair<std::string, std::string>> kShaped{
        {"sdr.gain_1090", {"\"auto\"", "\"30\""}},
        {"sdr.gain_978", {"\"auto\"", "\"30\""}},
        {"cues.tcp_connect", {"[\"h1:1\"]", "[\"h2:2\"]"}},
        {"cues.udp_targets", {"[\"h1:1\"]", "[\"h2:2\"]"}},
        {"paths.state_dir", {"\"/a\"", "\"/b\""}},
        {"paths.map_dir", {"\"/a\"", "\"/b\""}},
    };
    if (const auto it = kShaped.find(s.key); it != kShaped.end())
        return it->second;
    switch (s.type) {
    case KeyType::boolean: return {"true", "false"};
    case KeyType::integer:
    case KeyType::number: {
        const double lo = std::isinf(s.lo) ? 1.0 : std::ceil(s.lo);
        const double hi = std::isinf(s.hi) ? lo + 100.0 : s.hi;
        if (lo + 1.0 > hi) return {};
        const long a = static_cast<long>(lo), b = a + 1;
        return {std::to_string(a), std::to_string(b)};
    }
    case KeyType::string:
        if (s.values.size() >= 2) return {"\"" + s.values[0] + "\"", "\"" + s.values[1] + "\""};
        return {"\"x\"", "\"y\""};
    case KeyType::strings:
        if (s.values.size() >= 2) return {"[\"" + s.values[0] + "\"]", "[\"" + s.values[1] + "\"]"};
        return {"[\"x\"]", "[\"y\"]"};
    }
    return {};
}

// Keys with no old-format (skycue.conf) name.
const std::set<std::string> kNewKeys{"inputs.readsb.enable", "web.allow_restart"};

}  // namespace

BOOST_AUTO_TEST_CASE(every_key_has_value_text_comment_and_schema_row)
{
    const auto comments = exampleComments(slurp(SKYCUE_EXAMPLE_CONF));
    for (const KeySpec& s : keySpecs()) {
        const std::string key = s.key;
        BOOST_CHECK_NO_THROW(configValueText(Config(), key));
        if (kNoComment.count(key))
            BOOST_CHECK_MESSAGE(comments.count(key) == 0,
                                key << " now has an example comment: drop it from kNoComment");
        else
            BOOST_CHECK_MESSAGE(comments.count(key) == 1, key << " has no comment in the example");
    }

    const auto root = JsonObject::parse(configSchemaJson());
    BOOST_REQUIRE(root);
    const auto rows = root->getObjectArray("keys");
    BOOST_REQUIRE(rows);
    const auto& known = knownConfigKeys();
    BOOST_REQUIRE_EQUAL(rows->size(), known.size());
    for (std::size_t i = 0; i < known.size(); ++i) {
        const auto k = (*rows)[i].getString("key");
        BOOST_REQUIRE(k);
        BOOST_CHECK_EQUAL(*k, known[i]);
    }
}

BOOST_AUTO_TEST_CASE(every_restart_class_key_has_a_restart_table_entry)
{
    // restartKeysChanged skips a key with no entry, so a missing entry would
    // never show as pending.
    const auto& restart = restartConfigKeys();
    const std::set<std::string> in_table(restart.begin(), restart.end());
    for (const KeySpec& s : keySpecs())
        if (s.cls == KeyClass::restart)
            BOOST_CHECK_MESSAGE(in_table.count(s.key), s.key << " is restart-class but not in kRestartKeys");
    // restartConfigKeys() is derived from keySpecs(), so the table kRestartKeys is checked by
    // behaviour: two configs differing only in this key must report it as changed.
    for (const KeySpec& s : keySpecs()) {
        if (s.cls == KeyClass::live)
            continue;
        const std::string key = s.key;
        const auto pair = sampleLiterals(s);
        if (pair.first.empty()) {
            BOOST_CHECK_MESSAGE(kNoSample.count(key), key << ": no sample values; add to kNoSample or extend sampleLiterals");
            continue;
        }
        BOOST_CHECK_MESSAGE(!kNoSample.count(key), key << " now has samples: drop it from kNoSample");
        Config a, b;
        try {
            a = parseConfigText(skycue_test::tomlConf({{key, pair.first}}), "a", false);
            b = parseConfigText(skycue_test::tomlConf({{key, pair.second}}), "b", false);
        } catch (const ConfigError& e) {
            BOOST_ERROR(key << ": sample values rejected: " << e.what());
            continue;
        }
        const auto changed = restartKeysChanged(a, b);
        BOOST_CHECK_MESSAGE(std::find(changed.begin(), changed.end(), key) != changed.end(),
                            key << " is restart-class but has no usable entry in kRestartKeys");
    }
    const auto& known = knownConfigKeys();
    const std::set<std::string> known_set(known.begin(), known.end());
    for (const std::string& k : restart)
        BOOST_CHECK_MESSAGE(known_set.count(k), k << " is in the restart table but is not a known key");
}

BOOST_AUTO_TEST_CASE(legacy_conversion_covers_every_key_except_the_new_ones)
{
    const std::string text = convertOldConf(slurp(SKYCUE_TEST_DATA_DIR "/skycue_example_old.conf"), "old");
    const Config c = parseConfigText(text, "converted", false);
    std::set<std::string> got = c.file_keys;
    got.insert(kNewKeys.begin(), kNewKeys.end());
    const auto& known = knownConfigKeys();
    const std::set<std::string> want(known.begin(), known.end());
    for (const std::string& k : want)
        BOOST_CHECK_MESSAGE(got.count(k), k << " is not produced by the legacy conversion");
    for (const std::string& k : got)
        BOOST_CHECK_MESSAGE(want.count(k), k << " is produced by the conversion but is not a known key");
}

BOOST_AUTO_TEST_CASE(docs_name_every_key)
{
    const std::string docs = slurp(SKYCUE_DOCS_DIR "/operating.md");
    for (const std::string& key : knownConfigKeys()) {
        const auto dot = key.rfind('.');
        const std::string last = dot == std::string::npos ? key : key.substr(dot + 1);
        BOOST_CHECK_MESSAGE(docs.find(key) != std::string::npos || docs.find("`" + last + "`") != std::string::npos,
                            key << " is not named in docs/operating.md");
    }
}

BOOST_AUTO_TEST_CASE(page_config_copy_sample_uses_only_known_keys)
{
    // This is a sample of what the page's configText writes, not the whole key list.
    const auto doc = toml_lite::parse(slurp(SKYCUE_TEST_DATA_DIR "/config-copy.toml"), "config-copy.toml");
    const auto& known = knownConfigKeys();
    const std::set<std::string> known_set(known.begin(), known.end());
    for (const auto& e : doc.entries)
        BOOST_CHECK_MESSAGE(known_set.count(e.first), e.first << " in config-copy.toml is not a known key");
}
