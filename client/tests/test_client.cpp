#include <server/server.hpp>

#include <wheelie/client.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

using wheelie::Server;
using wheelie::ServerConfig;

namespace {

constexpr size_t file_size = 8 * 1024 * 1024;
constexpr unsigned int depth = 16;

std::vector<char> pattern(size_t size, int seed) {
  std::vector<char> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>(seed * 31 + i % 251);
  }
  return data;
}

// What the callback of a request reports.
struct Result {
  int calls = 0;
  int64_t res = 0;
};

void on_done(int64_t res, void *data) {
  Result *result = static_cast<Result *>(data);
  ++result->calls;
  result->res = res;
}

class ClientTest
    : public ::testing::TestWithParam<std::tuple<WioBackend, bool>> {
protected:
  void SetUp() override {
    char path[] = "/tmp/wheelie-client-test-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, file_size), 0);
    ::close(fd);
    _path = path;

    // The server is plain libc: it is the client that is under test, and
    // libc is there wherever the tests run.
    ServerConfig config;
    config.host = "127.0.0.1";
    config.file = _path;
    config.backend = WIO_BACKEND_LIBC;
    config.threads = 2;
    config.direct = false;
    _server = std::make_unique<Server>(config);
    _port = _server->start();

    int ret = open_client(depth);
    if (ret == -EPERM || ret == -ENOSYS) {
      GTEST_SKIP() << wio_backend_name(std::get<0>(GetParam()))
                   << " is not available: " << strerror(-ret);
    }
    ASSERT_EQ(ret, 0) << strerror(-ret);
  }

  void TearDown() override {
    if (_client != nullptr) {
      wheelie_client_delete(_client);
    }
    _server.reset();
    ::unlink(_path.c_str());
  }

  // Connects a client, or returns the error.
  int open_client(unsigned int client_depth) {
    WheelieClientOptions options = {};
    options.host = "127.0.0.1";
    options.port = _port;
    options.backend = std::get<0>(GetParam());
    options.multishot = std::get<1>(GetParam());
    options.depth = client_depth;
    return wheelie_client_create(&options, &_client);
  }

  // Waits until nothing is in flight.
  bool finish_everything() {
    for (int i = 0; i < 10000 && wheelie_client_inflight(_client) > 0; ++i) {
      if (wheelie_client_wait(_client, 1000) < 0) {
        return false;
      }
    }
    return wheelie_client_inflight(_client) == 0;
  }

  std::string _path;
  std::unique_ptr<Server> _server;
  uint16_t _port = 0;
  WheelieClient *_client = nullptr;
};

} // namespace

TEST_P(ClientTest, WriteThenRead) {
  std::vector<char> data = pattern(4096, 1);
  std::vector<char> back(4096);
  Result written, read;

  ASSERT_EQ(wheelie_client_pwrite(_client, data.data(), data.size(), 8192, 0,
                                  on_done, &written),
            0);
  EXPECT_EQ(wheelie_client_inflight(_client), 1u);
  ASSERT_TRUE(finish_everything());
  EXPECT_EQ(written.calls, 1);
  EXPECT_EQ(written.res, 4096);

  ASSERT_EQ(wheelie_client_pread(_client, back.data(), back.size(), 8192,
                                 on_done, &read),
            0);
  ASSERT_TRUE(finish_everything());
  EXPECT_EQ(read.calls, 1);
  EXPECT_EQ(read.res, 4096);
  EXPECT_EQ(back, data);
}

TEST_P(ClientTest, DurableWrite) {
  std::vector<char> data = pattern(8192, 2);
  std::vector<char> back(8192);
  Result written, read;

  ASSERT_EQ(wheelie_client_pwrite(_client, data.data(), data.size(), 0,
                                  WHEELIE_FLAG_SYNC, on_done, &written),
            0);
  ASSERT_TRUE(finish_everything());
  EXPECT_EQ(written.res, 8192);

  ASSERT_EQ(wheelie_client_pread(_client, back.data(), back.size(), 0, on_done,
                                 &read),
            0);
  ASSERT_TRUE(finish_everything());
  EXPECT_EQ(back, data);
}

TEST_P(ClientTest, RequestsSubmittedTogetherAreAllAnswered) {
  std::vector<std::vector<char>> data;
  std::vector<std::vector<char>> back(depth, std::vector<char>(4096));
  std::vector<Result> written(depth), read(depth);

  for (unsigned int i = 0; i < depth; ++i) {
    data.push_back(pattern(4096, 10 + i));
    ASSERT_EQ(wheelie_client_pwrite(_client, data[i].data(), 4096, i * 4096, 0,
                                    on_done, &written[i]),
              0);
  }
  EXPECT_EQ(wheelie_client_inflight(_client), depth);
  ASSERT_TRUE(finish_everything());
  for (unsigned int i = 0; i < depth; ++i) {
    EXPECT_EQ(written[i].res, 4096) << i;
  }

  for (unsigned int i = 0; i < depth; ++i) {
    ASSERT_EQ(wheelie_client_pread(_client, back[i].data(), 4096, i * 4096,
                                   on_done, &read[i]),
              0);
  }
  ASSERT_TRUE(finish_everything());
  for (unsigned int i = 0; i < depth; ++i) {
    EXPECT_EQ(read[i].res, 4096) << i;
    EXPECT_EQ(back[i], data[i]) << i;
  }
}

namespace {

// Keeps `depth` requests in flight until `total` have been submitted: each one
// that finishes submits the next from its callback.
struct Stream {
  WheelieClient *client;
  std::vector<char> data;
  std::vector<std::vector<char>> buffers;
  int total;
  int submitted = 0;
  int finished = 0;
  int failed = 0;

  static void on_read(int64_t res, void *data_) {
    Stream *self = static_cast<Stream *>(data_);
    ++self->finished;
    if (res != 4096) {
      ++self->failed;
    }
    self->submit_more();
  }

  void submit_more() {
    while (submitted < total) {
      std::vector<char> &buffer = buffers[submitted % buffers.size()];
      if (wheelie_client_pread(client, buffer.data(), 4096,
                               (submitted % 64) * 4096, on_read, this) != 0) {
        return;
      }
      ++submitted;
    }
  }
};

} // namespace

TEST_P(ClientTest, CallbacksKeepTheQueueFull) {
  Stream stream;
  stream.client = _client;
  stream.total = 500;
  stream.buffers.assign(depth, std::vector<char>(4096));

  stream.submit_more();
  EXPECT_EQ(stream.submitted, static_cast<int>(depth));
  for (int i = 0; i < 10000 && stream.finished < stream.total; ++i) {
    ASSERT_GE(wheelie_client_wait(_client, 1000), 0);
  }
  EXPECT_EQ(stream.finished, 500);
  EXPECT_EQ(stream.submitted, 500);
  EXPECT_EQ(stream.failed, 0);
}

TEST_P(ClientTest, LargeWriteAndRead) {
  std::vector<char> data = pattern(WHEELIE_MAX_PAYLOAD_SIZE, 5);
  std::vector<char> back(data.size());
  Result written, read;

  ASSERT_EQ(wheelie_client_pwrite(_client, data.data(), data.size(), 4096, 0,
                                  on_done, &written),
            0);
  ASSERT_TRUE(finish_everything());
  ASSERT_EQ(written.res, static_cast<int64_t>(data.size()));

  ASSERT_EQ(wheelie_client_pread(_client, back.data(), back.size(), 4096,
                                 on_done, &read),
            0);
  ASSERT_TRUE(finish_everything());
  ASSERT_EQ(read.res, static_cast<int64_t>(back.size()));
  EXPECT_TRUE(back == data);
}

TEST_P(ClientTest, ReadPastTheEndIsShort) {
  std::vector<char> data = pattern(2048, 3);
  std::vector<char> tail(4096), past(4096);
  Result written, short_read, empty_read;

  ASSERT_EQ(wheelie_client_pwrite(_client, data.data(), data.size(),
                                  file_size - 2048, 0, on_done, &written),
            0);
  ASSERT_TRUE(finish_everything());

  ASSERT_EQ(wheelie_client_pread(_client, tail.data(), tail.size(),
                                 file_size - 2048, on_done, &short_read),
            0);
  ASSERT_EQ(wheelie_client_pread(_client, past.data(), past.size(), file_size,
                                 on_done, &empty_read),
            0);
  ASSERT_TRUE(finish_everything());
  EXPECT_EQ(short_read.res, 2048);
  EXPECT_EQ(std::memcmp(tail.data(), data.data(), 2048), 0);
  EXPECT_EQ(empty_read.res, 0);
}

TEST_P(ClientTest, ASubmissionThatCannotBeMadeSaysSo) {
  wheelie_client_delete(_client);
  _client = nullptr;
  ASSERT_EQ(open_client(2), 0);
  std::vector<char> data(4096), big(WHEELIE_MAX_PAYLOAD_SIZE + 1);
  Result a, b, c;

  EXPECT_EQ(
      wheelie_client_pread(_client, data.data(), data.size(), 0, on_done, &a),
      0);
  EXPECT_EQ(
      wheelie_client_pread(_client, data.data(), data.size(), 0, on_done, &b),
      0);
  EXPECT_EQ(
      wheelie_client_pread(_client, data.data(), data.size(), 0, on_done, &c),
      -EBUSY);
  ASSERT_TRUE(finish_everything());
  EXPECT_EQ(c.calls, 0);

  EXPECT_EQ(
      wheelie_client_pread(_client, big.data(), big.size(), 0, on_done, &a),
      -EMSGSIZE);
  EXPECT_EQ(
      wheelie_client_pwrite(_client, big.data(), big.size(), 0, 0, on_done, &a),
      -EMSGSIZE);
  EXPECT_EQ(wheelie_client_pwrite(_client, data.data(), data.size(), 0, 0x80,
                                  on_done, &a),
            -EINVAL);
  EXPECT_EQ(
      wheelie_client_pread(_client, data.data(), data.size(), 0, on_done, &a),
      0);
  ASSERT_TRUE(finish_everything());
}

TEST_P(ClientTest, WaitWithNothingToWaitForTimesOut) {
  EXPECT_EQ(wheelie_client_wait(_client, 0), 0);
  EXPECT_EQ(wheelie_client_wait(_client, 10), 0);
}

TEST_P(ClientTest, WaitRunsTheCallbacksOfFinishedRequests) {
  std::vector<char> buffer(4096);
  Result a, b;
  ASSERT_EQ(wheelie_client_pread(_client, buffer.data(), buffer.size(), 0,
                                 on_done, &a),
            0);
  ASSERT_EQ(wheelie_client_pread(_client, buffer.data(), buffer.size(), 4096,
                                 on_done, &b),
            0);

  // Nothing runs outside wait: submitting only queues the requests.
  EXPECT_EQ(a.calls + b.calls, 0);
  for (int i = 0; i < 10000 && a.calls + b.calls < 2; ++i) {
    ASSERT_EQ(wheelie_client_wait(_client, 1000), 0);
  }
  EXPECT_EQ(a.calls, 1);
  EXPECT_EQ(b.calls, 1);
}

TEST_P(ClientTest, DeletingCancelsWhatIsInFlight) {
  std::vector<char> buffer(4096);
  Result a, b;
  ASSERT_EQ(wheelie_client_pread(_client, buffer.data(), buffer.size(), 0,
                                 on_done, &a),
            0);
  ASSERT_EQ(wheelie_client_pwrite(_client, buffer.data(), buffer.size(), 0, 0,
                                  on_done, &b),
            0);

  wheelie_client_delete(_client);
  _client = nullptr;
  EXPECT_EQ(a.calls, 1);
  EXPECT_EQ(b.calls, 1);
  EXPECT_EQ(a.res, -ECANCELED);
  EXPECT_EQ(b.res, -ECANCELED);
}

TEST_P(ClientTest, ServerGoingAwayFailsTheRequestsAndTheOnesAfter) {
  std::vector<char> buffer(4096);
  Result a, b;
  ASSERT_EQ(wheelie_client_pread(_client, buffer.data(), buffer.size(), 0,
                                 on_done, &a),
            0);

  _server->stop();
  ASSERT_TRUE(finish_everything());
  EXPECT_EQ(a.calls, 1);
  EXPECT_LT(a.res, 0);
  // It reports what broke the connection, not the cancellation that the client
  // itself uses to wind its connection down.
  EXPECT_NE(a.res, -ECANCELED);

  // The connection is broken for good: nothing more is accepted.
  EXPECT_LT(wheelie_client_pread(_client, buffer.data(), buffer.size(), 0,
                                 on_done, &b),
            0);
  EXPECT_EQ(b.calls, 0);
}

TEST_P(ClientTest, TwoClientsDoNotGetInEachOthersWay) {
  WheelieClient *other = nullptr;
  WheelieClientOptions options = {};
  options.host = "127.0.0.1";
  options.port = _port;
  options.backend = std::get<0>(GetParam());
  options.multishot = std::get<1>(GetParam());
  options.depth = depth;
  ASSERT_EQ(wheelie_client_create(&options, &other), 0);

  std::vector<char> mine = pattern(4096, 7), theirs = pattern(4096, 8);
  std::vector<char> back_mine(4096), back_theirs(4096);
  Result a, b, c, d;
  ASSERT_EQ(
      wheelie_client_pwrite(_client, mine.data(), 4096, 0, 0, on_done, &a), 0);
  ASSERT_EQ(
      wheelie_client_pwrite(other, theirs.data(), 4096, 4096, 0, on_done, &b),
      0);
  ASSERT_TRUE(finish_everything());
  for (int i = 0; i < 10000 && wheelie_client_inflight(other) > 0; ++i) {
    ASSERT_GE(wheelie_client_wait(other, 1000), 0);
  }

  ASSERT_EQ(wheelie_client_pread(other, back_mine.data(), 4096, 0, on_done, &c),
            0);
  ASSERT_EQ(wheelie_client_pread(_client, back_theirs.data(), 4096, 4096,
                                 on_done, &d),
            0);
  ASSERT_TRUE(finish_everything());
  for (int i = 0; i < 10000 && wheelie_client_inflight(other) > 0; ++i) {
    ASSERT_GE(wheelie_client_wait(other, 1000), 0);
  }
  EXPECT_EQ(back_mine, mine);
  EXPECT_EQ(back_theirs, theirs);
  wheelie_client_delete(other);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, ClientTest,
    ::testing::Combine(::testing::Values(WIO_BACKEND_LIBC, WIO_BACKEND_ALIBC,
                                         WIO_BACKEND_URING),
                       ::testing::Bool()),
    [](const ::testing::TestParamInfo<std::tuple<WioBackend, bool>> &info) {
      return std::string(wio_backend_name(std::get<0>(info.param))) +
             (std::get<1>(info.param) ? "_multishot" : "_single");
    });

TEST(ClientConnect, RefusedConnectionIsAnError) {
  // A port nobody listens on: bound and closed again.
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
            0);
  socklen_t length = sizeof(address);
  ASSERT_EQ(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length),
            0);
  ::close(fd);

  WheelieClientOptions options = {};
  options.host = "127.0.0.1";
  options.port = ntohs(address.sin_port);
  options.backend = WIO_BACKEND_LIBC;
  options.depth = 4;
  WheelieClient *client = nullptr;
  EXPECT_EQ(wheelie_client_create(&options, &client), -ECONNREFUSED);
  EXPECT_EQ(client, nullptr);
}

TEST(ClientConnect, UnknownHostIsAnError) {
  WheelieClientOptions options = {};
  options.host = "no-such-host.invalid";
  options.port = 1;
  options.backend = WIO_BACKEND_LIBC;
  options.depth = 4;
  WheelieClient *client = nullptr;
  EXPECT_LT(wheelie_client_create(&options, &client), 0);
  EXPECT_EQ(client, nullptr);
}

TEST(ClientConnect, BadOptionsAreRefused) {
  WheelieClient *client = nullptr;
  EXPECT_EQ(wheelie_client_create(nullptr, &client), -EINVAL);

  WheelieClientOptions options = {};
  options.host = "127.0.0.1";
  options.port = 1;
  options.backend = WIO_BACKEND_LIBC;
  options.depth = 0;
  EXPECT_EQ(wheelie_client_create(&options, &client), -EINVAL);
}
