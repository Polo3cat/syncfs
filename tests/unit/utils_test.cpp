#include <gtest/gtest.h>
#include <stdexcept>
#include <utility>
#include <utils.h>

// For debugging remember to call
// set target.disable-aslr false
// on lldb
TEST(Utils, ParseHostPort) {
  ASSERT_EQ(utils::parse_host_port("1.1.1.1:1234"),
            (std::pair{"1.1.1.1", 1234}));
}

TEST(Utils, EmptyHostThrows) {
  ASSERT_THROW(utils::parse_host_port(":1234"), std::invalid_argument);
}

TEST(Utils, EmptyPortThrows) {
  ASSERT_THROW(utils::parse_host_port("1.1.1.1:"), std::invalid_argument);
}

TEST(Utils, MissingColonThrows) {
  ASSERT_THROW(utils::parse_host_port("1.1.1.1 1234"), std::out_of_range);
}
