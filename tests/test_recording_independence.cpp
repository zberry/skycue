#define BOOST_TEST_MODULE skycued_recording_independence
#include <boost/test/unit_test.hpp>

#include "engine_test_support.hpp"
#include "readsb_json_source.hpp"

#include <set>

using namespace enginetest;

// The received recording holds everything the source delivered, whatever the
// filters decide: with every aircraft of the capture block-listed, nothing is
// sent, yet every observation is recorded.
BOOST_AUTO_TEST_CASE(received_stream_unaffected_by_filters)
{
    size_t parseable = 0;
    std::set<std::string> hexes;
    {
        std::ifstream in(kTestData);
        std::string line;
        while (std::getline(in, line)) {
            if (const auto obs = skycued::parseReadsbLine(line)) {
                ++parseable;
                char buf[8];
                std::snprintf(buf, sizeof buf, "%06x", static_cast<unsigned>(obs->id.address));
                hexes.insert(buf);
            }
        }
    }
    BOOST_REQUIRE(parseable > 1000u);

    TempDir tmp;
    {
        std::ofstream block(tmp.path + "/block.txt");
        for (const auto& h : hexes)
            block << h << "\n";
    }
    Config cfg = baseConfig();
    cfg.block_list_path = tmp.path + "/block.txt";
    {
        EngineFx fx(cfg, replay(0), 10s, tmp.path);  // unpaced: the whole capture at once
        const auto deadline = steady::now() + 10s;
        while (fx.stats().observations < parseable && steady::now() < deadline)
            std::this_thread::sleep_for(20ms);
        const auto st = fx.stats();
        BOOST_TEST(st.observations == parseable);
        BOOST_TEST(st.sent_updates == 0u);
        BOOST_TEST(st.tracks_forwarded == 0u);
        BOOST_TEST(st.tracks_total > 10u);
        BOOST_TEST(st.recorder_dropped_received == 0u);
        fx.stop();
    }
    const std::string received = streamText(tmp.path, "received");
    const std::string sent = streamText(tmp.path, "sent");
    BOOST_TEST(countOf(received, "\"type\":\"obs\"") == parseable);
    BOOST_TEST(countOf(sent, "\"event\":\"update\"") == 0u);
    BOOST_TEST(countOf(sent, "\"event\":\"drop\"") == 0u);
    BOOST_TEST(countOf(sent, "\"event\":\"filter_change\"") == 1u);
}
