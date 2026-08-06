#include <boost/ut.hpp>

import test_at;
import test_modem;
import test_pdu;
import test_sms;

int main(int argc, char *argv[]) {
  tests::register_at_tests();
  tests::register_pdu_tests();
  tests::register_sms_tests();
  tests::register_modem_tests();
  // Boost.UT 的 run_cfg 使用 C ABI 风格 argv；仅在入口边界传递该指针。
  const auto run_config = boost::ut::run_cfg{
      .report_errors = false,
      .argc = argc,
      .argv = const_cast<const char **>(argv),
  };
  const bool failed = boost::ut::cfg<>.run(run_config);
  return failed ? 1 : 0;
}
