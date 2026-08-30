module;

#include <cstdint>
#include <string>

export module test_modem;

import asterisk;
import modem;
import support;

namespace tests {

export void register_modem_tests() {
  "Modem and Asterisk state"_test = [] {
    require(modem::parse_csq_dbm("+CSQ: 15,99") == -83,
            "CSQ should convert RSSI to dBm");
    require(modem::parse_csq_dbm("+CSQ: 99,99") == 0,
            "unknown RSSI should map to zero");
    require(modem::first_nonzero_registration("+CEREG: 0,0", "+CREG: 0,5") == 5,
            "registration parser should prefer the first non-zero status");
    require(modem::parse_operator("+COPS: 0,0,\"Example\"") == "Example",
            "operator parser should return quoted operator name");
    std::string mode;
    std::string duplex;
    std::string band;
    uint32_t channel = 0;
    modem::parse_qnwinfo("+QNWINFO: \"FDD LTE\",\"Example\",\"LTE B3\",123",
                         mode, duplex, band, channel);
    require(mode == "LTE" && duplex == "FDD" && band == "B3" && channel == 123,
            "QNWINFO parser should split radio mode and channel");
    const auto data_mode = modem::work_mode_from_name("internet");
    require(data_mode.has_value() && *data_mode == modem::WorkMode::Data,
            "internet alias should select data mode");
    require(!modem::work_mode_from_name("unknown").has_value(),
            "unknown work mode should be rejected");
    require(asterisk::state_name(asterisk::State::InCall) == "in_call",
            "Asterisk state names must be stable wire values");
  };
}

} // namespace tests
