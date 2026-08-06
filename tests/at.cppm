module;

#include <boost/ut.hpp>
#include <string>
#include <vector>

export module test_at;

import support;
import usb;

namespace tests {

export void register_at_tests() {
  using namespace boost::ut;
  "AT parsing"_test = [] {
    const std::string response = "\r\n +CSQ: 17,99\r\n\r\nOK\r\n";
    require(usb::at_response_complete(response), "AT response should complete");
    require(!usb::at_response_is_error(response),
            "OK response is not an error");
    require(usb::parse_ints("+CSQ: 17,99") == std::vector<int>{17, 99},
            "AT integer parsing must preserve both fields");
    require(usb::normalize_at_response(response) == "+CSQ: 17,99\r\nOK",
            "AT normalization should remove framing whitespace");
    require(usb::at_response_is_error("+CME ERROR: 10"),
            "CME ERROR must be recognized");
    require(usb::at_response_has_prompt("> "),
            "AT prompt should be recognized after trailing whitespace");
    require(usb::parse_quoted("+COPS: 0,0,\"Example\"") ==
                std::vector<std::string>{"Example"},
            "quoted AT fields should be extracted");
  };
}

} // namespace tests
