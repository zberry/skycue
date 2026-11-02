#include "fisb/apdu.hpp"

#include "fisb/bit_reader.hpp"

namespace skycued {
namespace fisb {

std::optional<ApduHeader> parseApdu(const uint8_t* p, std::size_t n, bool experimental_time_opts,
                                    ApduStatus* status)
{
    ApduStatus dummy;
    ApduStatus& st = status ? *status : dummy;
    st = ApduStatus::truncated;

    BitReader r(p, n);
    ApduHeader h;
    const auto a = r.u(1), g = r.u(1), pf = r.u(1);
    const auto pid = r.u(11);
    const auto s = r.u(1);
    const auto topt = r.u8(2);
    if (r.overrun())
        return std::nullopt;
    h.a_flag = *a != 0;
    h.g_flag = *g != 0;
    h.p_flag = *pf != 0;
    h.product_id = static_cast<uint16_t>(*pid);
    h.s_flag = h.segmented = *s != 0;
    h.t_opt = *topt;

    const bool has_date = (h.t_opt & 2u) != 0;
    const bool has_seconds = (h.t_opt & 1u) != 0;
    if (has_seconds && !experimental_time_opts) {
        st = ApduStatus::experimental_time_option;
        return std::nullopt;
    }
    if (has_date) {
        h.month = r.u8(4);
        h.day = r.u8(5);
    }
    const auto hours = r.u8(5);
    const auto minutes = r.u8(6);
    if (has_seconds)
        h.seconds = r.u8(6);
    if (h.segmented) {
        const auto file_id = r.u(10);
        const auto count = r.u(9);
        const auto number = r.u(9);
        if (!r.overrun()) {
            h.apdu_number = static_cast<uint16_t>(*file_id);
            h.segment_count = static_cast<uint16_t>(*count);
            h.segment_number = static_cast<uint16_t>(*number);
        }
    }
    if (r.overrun())
        return std::nullopt;
    h.hours = *hours;
    h.minutes = *minutes;
    h.header_bytes = r.bytesConsumed();
    st = ApduStatus::ok;
    return h;
}

}  // namespace fisb
}  // namespace skycued
