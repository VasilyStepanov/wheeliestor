#include <wheelie/client.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace {

struct Parsed {
  int ret;
  std::string host;
  uint16_t port;
};

Parsed parse(const char *location, size_t host_size = 256) {
  char host[256] = "untouched";
  uint16_t port = 0;
  int ret = wheelie_location_parse(location, host, host_size, &port);
  return {ret, host, port};
}

} // namespace

TEST(Location, AddressAndPort) {
  Parsed parsed = parse("wheelie://127.0.0.1:39900");
  EXPECT_EQ(parsed.ret, 0);
  EXPECT_EQ(parsed.host, "127.0.0.1");
  EXPECT_EQ(parsed.port, 39900u);
}

TEST(Location, NameAndPort) {
  Parsed parsed = parse("wheelie://server.example:1");
  EXPECT_EQ(parsed.ret, 0);
  EXPECT_EQ(parsed.host, "server.example");
  EXPECT_EQ(parsed.port, 1u);

  parsed = parse("wheelie://server:65535");
  EXPECT_EQ(parsed.ret, 0);
  EXPECT_EQ(parsed.port, 65535u);
}

TEST(Location, RejectsWhatIsNotALocationOfAServer) {
  for (const char *location :
       {"", "127.0.0.1:39900", "wheelie:/127.0.0.1:39900",
        "wheelie://127.0.0.1", "wheelie://:39900",
        "wheelie://127.0.0.1:", "wheelie://127.0.0.1:0",
        "wheelie://127.0.0.1:65536", "wheelie://127.0.0.1:-1",
        "wheelie://127.0.0.1:+80", "wheelie://127.0.0.1: 80",
        "wheelie://127.0.0.1:80x", "wheelie://host/path:80",
        "ost://127.0.0.1:39900", "WHEELIE://127.0.0.1:39900"}) {
    EXPECT_EQ(parse(location).ret, -EINVAL) << location;
  }
}

TEST(Location, HostHasToFit) {
  // Nine characters and the terminating zero.
  EXPECT_EQ(parse("wheelie://127.0.0.1:80", 10).ret, 0);
  EXPECT_EQ(parse("wheelie://127.0.0.1:80", 9).ret, -EINVAL);
}

TEST(Location, ANullArgumentIsRefused) {
  char host[16];
  uint16_t port;
  EXPECT_EQ(wheelie_location_parse(nullptr, host, sizeof(host), &port),
            -EINVAL);
  EXPECT_EQ(wheelie_location_parse("wheelie://h:1", nullptr, 16, &port),
            -EINVAL);
  EXPECT_EQ(
      wheelie_location_parse("wheelie://h:1", host, sizeof(host), nullptr),
      -EINVAL);
}
