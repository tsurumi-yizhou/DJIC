module;

#include <boost/ut.hpp>
#include <string>
#include <string_view>

export module support;

namespace tests {

export inline void require(bool condition, std::string_view message) {
  boost::ut::expect(condition) << message;
}

} // namespace tests
