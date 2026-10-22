#include <wheelio/wheelio.h>

#include <gtest/gtest.h>

#include <cerrno>

TEST(Backend, NameRoundTrip) {
  for (WioBackend backend :
       {WIO_BACKEND_LIBC, WIO_BACKEND_ALIBC, WIO_BACKEND_URING}) {
    const char *name = wio_backend_name(backend);
    ASSERT_NE(name, nullptr);

    WioBackend parsed = WIO_BACKEND_LIBC;
    ASSERT_EQ(wio_backend_from_name(name, &parsed), 0);
    EXPECT_EQ(parsed, backend);
  }
}

TEST(Backend, UnknownNameIsRejected) {
  WioBackend backend = WIO_BACKEND_URING;
  EXPECT_EQ(wio_backend_from_name("epoll", &backend), -EINVAL);
  EXPECT_EQ(wio_backend_from_name(nullptr, &backend), -EINVAL);
  EXPECT_EQ(backend, WIO_BACKEND_URING);
}

TEST(Backend, InvalidValueHasNoName) {
  EXPECT_EQ(wio_backend_name(static_cast<WioBackend>(-1)), nullptr);
  EXPECT_EQ(wio_backend_name(static_cast<WioBackend>(99)), nullptr);
}
