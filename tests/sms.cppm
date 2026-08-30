module;

#include <string>

export module test_sms;

import sms;
import smscodec;
import support;

namespace tests {

export void register_sms_tests() {
  "SMS logic"_test = [] {
    require(sms::extract_sms_code("您的验证码： 482913") == "482913",
            "Chinese verification code should be extracted");
    require(sms::extract_sms_code("Code: 1234, expires soon") == "1234",
            "keyword followed by punctuation should be accepted");
    require(sms::extract_sms_code("739201 otp") == "739201",
            "code preceding an English keyword should be extracted");
    require(sms::extract_sms_code("订单号 123456789012").empty(),
            "unrelated long numbers must not be treated as OTP");

    sms::Reassembler reassembler;
    smscodec::ConcatInfo concat{true, 7, 8, 2, 2};
    std::string full;
    require(!reassembler.add("10086", concat, "world", full),
            "last fragment alone must remain incomplete");
    concat.seq = 1;
    require(reassembler.add("10086", concat, "hello ", full),
            "all fragments should complete reassembly");
    require(full == "hello world", "fragments must be ordered by sequence");
    require(!reassembler.add("10086", concat, "duplicate", full),
            "a completed message must not be re-emitted");
    smscodec::ConcatInfo plain{};
    require(reassembler.add("10086", plain, "single", full) && full == "single",
            "non-concatenated SMS should pass through directly");
  };
}

} // namespace tests
