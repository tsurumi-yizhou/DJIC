module;

#include <array>
#include <boost/ut.hpp>
#include <cstdint>
#include <span>
#include <string>

export module test_pdu;

import smscodec;
import support;

namespace tests {

export void register_pdu_tests() {
  using namespace boost::ut;
  "PDU codec"_test = [] {
    const std::string text = "Hello";
    const auto encoded = smscodec::encode_gsm7_septets(text);
    require(encoded.has_value(), "ASCII text should encode as GSM-7");
    require(smscodec::decode_gsm7_septets(*encoded) == text,
            "GSM-7 round trip should preserve ASCII text");
    const auto packed = smscodec::pack_septets(*encoded);
    require(smscodec::unpack_septets(std::span<const uint8_t>(packed),
                                     encoded->size()) == *encoded,
            "GSM-7 packing round trip should preserve septets");
    const std::array<uint8_t, 1> truncated_packed{0xFF};
    require(smscodec::unpack_septets(truncated_packed, 3).size() == 1,
            "truncated GSM-7 input must stop at the available bytes");
    require(!smscodec::encode_gsm7_septets("你好").has_value(),
            "non-GSM characters should request UCS2 fallback");
    const auto ucs2 = smscodec::encode_ucs2("你好");
    require(smscodec::decode_ucs2(ucs2) == "你好",
            "UCS2 round trip should preserve Unicode text");

    uint8_t ton_npi = 0;
    const auto address = smscodec::pack_phone("+8613800138000", ton_npi);
    require(ton_npi == 0x91, "international number must use international TON");
    require(smscodec::unpack_phone(address) == "8613800138000",
            "phone address round trip should preserve digits");
    require(smscodec::unpack_phone(std::array<uint8_t, 1>{0x02}).empty(),
            "truncated phone address must be rejected");
    const std::array<uint8_t, 7> scts{0x62, 0x80, 0x70, 0x21, 0x43, 0x65, 0x00};
    require(smscodec::decode_scts(scts) == "2026-08-07 12:34:56",
            "SCTS BCD fields should decode into an ISO timestamp");
    const std::array<uint8_t, 2> short_scts{0x00, 0x01};
    require(smscodec::decode_scts(short_scts).empty(),
            "truncated SCTS must be rejected");
    const auto single_segment =
        smscodec::build_submit_tpdus("13800138000", "Hello", false);
    require(single_segment.size() == 1 && single_segment.front().length > 0,
            "short GSM-7 SMS should produce one TPDU");
    const auto multipart = smscodec::build_submit_tpdus(
        "13800138000", std::string(200, 'A'), false);
    require(multipart.size() > 1,
            "long GSM-7 SMS should be split into multiple TPDUs");
    const auto unicode_segment =
        smscodec::build_submit_tpdus("13800138000", "你好", false);
    require(unicode_segment.size() == 1 &&
                unicode_segment.front().tpdu.size() > 11 &&
                unicode_segment.front().tpdu[11] == 0x08,
            "Unicode SMS should select UCS2 data coding");
  };
}

} // namespace tests
