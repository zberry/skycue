// libFuzzer harness: the parsers of the operator's files, all on text in
// memory (no file is read or written). Mode byte (fuzz_util.hpp splitMode),
// then the payload:
//   0  parseConfigText (skycue.toml, config.hpp) and toml_lite::parse
//      (skycue.toml, toml_lite.hpp); a document that parses has formatValue of
//      each value read back equal
//   1  parseInputsFile (skycue.inputs.json, inputs/inputs_file.hpp); a file
//      that parses comes back equal through inputsFileJson
//   2  parseOverlay (skycue.runtime.json, runtime_config.hpp); an overlay that
//      parses comes back equal through overlayJson
//   3  fisb::LocationIndex::loadLearned (fisb-locations.json); after a load
//      that succeeds, loading learnedJson() again leaves it unchanged
//   4  fisb::LocationIndex::loadOperatorCsv (the LOCATIONS_FILE CSV); the
//      count it returns is at most the number of lines
//   5  IdList::parse (allow and block lists, filters.hpp)
// Each mode catches only the exception type its parser documents; anything
// else escaping is a finding. Seeds: seeds/config_files/ (docs/hardening.md
// "Fuzzing").

#include "config.hpp"
#include "filters.hpp"
#include "fisb/location_index.hpp"
#include "fuzz_util.hpp"
#include "inputs/input_def.hpp"
#include "inputs/inputs_file.hpp"
#include "runtime_config.hpp"
#include "toml_lite.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

void configText(const std::string& payload)
{
    try {
        (void)skycued::parseConfigText(payload, "fuzz");
    } catch (const skycued::ConfigError&) {
    }
    skycued::toml_lite::Document doc;
    try {
        doc = skycued::toml_lite::parse(payload, "fuzz");
    } catch (const skycued::toml_lite::Error&) {
        return;
    }
    for (const auto& kv : doc.entries) {
        const auto& v = kv.second.value;
        if (const auto* d = std::get_if<double>(&v); d && !std::isfinite(*d))
            continue;  // the parser never gives one; formatValue would refuse it
        const auto back = skycued::toml_lite::parse("k = " + skycued::toml_lite::formatValue(v) + "\n", "fuzz");
        FUZZ_CHECK(back.entries.at("k").value == v);
    }
}

void inputsFile(const std::string& payload)
{
    skycued::InputsFile f;
    try {
        f = skycued::parseInputsFile(payload);
    } catch (const skycued::InputDefError&) {
        return;
    }
    const skycued::InputsFile back = skycued::parseInputsFile(skycued::inputsFileJson(f));
    FUZZ_CHECK(back.inputs.size() == f.inputs.size());
    for (std::size_t i = 0; i < f.inputs.size(); ++i)
        FUZZ_CHECK(back.inputs[i] == f.inputs[i]);
    FUZZ_CHECK(back.builtin_enabled == f.builtin_enabled);
}

void overlay(const std::string& payload)
{
    skycued::Overlay o;
    try {
        o = skycued::parseOverlay(payload);
    } catch (const std::runtime_error&) {
        return;
    }
    FUZZ_CHECK(skycued::parseOverlay(skycued::overlayJson(o)) == o);
}

void learned(const std::string& payload)
{
    skycued::fisb::LocationIndex index;
    try {
        index.loadLearned(payload);
    } catch (const std::runtime_error&) {
        return;
    }
    const std::string once = index.learnedJson();
    index.loadLearned(once);
    FUZZ_CHECK(index.learnedJson() == once);
}

void operatorCsv(const std::string& payload)
{
    skycued::fisb::LocationIndex index;
    const std::size_t n = index.loadOperatorCsv(payload);
    const auto lines = static_cast<std::size_t>(std::count(payload.begin(), payload.end(), '\n')) + 1;
    FUZZ_CHECK(n <= lines);
    FUZZ_CHECK(index.operatorCount() <= n);
}

void idList(const std::string& payload)
{
    try {
        (void)skycued::IdList::parse(payload, "fuzz");
    } catch (const std::runtime_error&) {
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size)
{
    skycuefuzz::canary(data, size);
    skycuefuzz::quietLogs();
    const auto in = skycuefuzz::splitMode(data, size, 6);
    switch (in.mode) {
    case 0:
        configText(in.payload);
        break;
    case 1:
        inputsFile(in.payload);
        break;
    case 2:
        overlay(in.payload);
        break;
    case 3:
        learned(in.payload);
        break;
    case 4:
        operatorCsv(in.payload);
        break;
    default:
        idList(in.payload);
        break;
    }
    return 0;
}
