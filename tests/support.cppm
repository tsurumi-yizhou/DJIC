module;

#include <boost/ut.hpp>
#include <string>
#include <string_view>

export module support;

namespace tests {

export inline void require(bool condition, std::string_view message) {
  boost::ut::expect(condition) << message;
}

export using boost::ut::literals::operator""_test;

export inline bool run_tests(int argc, const char **argv) {
  const auto run_config = boost::ut::run_cfg{
      .report_errors = false,
      .argc = argc,
      .argv = argv,
  };
  return boost::ut::cfg<>.run(run_config);
}

} // namespace tests
