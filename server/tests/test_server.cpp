#include <server/server.hpp>

#include <wheelie/protocol.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

using wheelie::Server;
using wheelie::ServerConfig;

namespace {

constexpr size_t file_size = 8 * 1024 * 1024;

std::vector<char> pattern(size_t size, int seed) {
  std::vector<char> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>(seed * 31 + i % 251);
  }
  return data;
}

// A plain blocking client of the protocol.
class Client {
public:
  struct Response {
    uint64_t cid;
    int64_t res;
    std::vector<char> data;
  };

  explicit Client(uint16_t port) {
    _fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(_fd, 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(
        ::connect(_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
        0);
    timeval timeout = {10, 0};
    setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;
  ~Client() { ::close(_fd); }

  void send_request(uint64_t cid, uint8_t op, uint64_t offset, uint64_t size,
                    const std::vector<char> &payload = {}) {
    WheelieRequestHeader header = {cid, offset, size, op};
    std::vector<char> bytes(sizeof(header) + payload.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
    size_t sent = 0;
    while (sent < bytes.size()) {
      ssize_t n =
          ::send(_fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
      ASSERT_GT(n, 0) << strerror(errno);
      sent += n;
    }
    _reads[cid] = op == WHEELIE_OP_READ;
  }

  // The next response; the data of a read follows its header.
  Response read_response() {
    Response response = {};
    WheelieResponseHeader header;
    if (!read_exactly(&header, sizeof(header))) {
      response.res = INT64_MIN;
      return response;
    }
    response.cid = header.cid;
    response.res = header.res;
    if (_reads[header.cid] && header.res > 0) {
      response.data.resize(header.res);
      EXPECT_TRUE(read_exactly(response.data.data(), response.data.size()));
    }
    return response;
  }

  // Says that no more requests are coming, and keeps listening.
  void finish_sending() { ::shutdown(_fd, SHUT_WR); }

  // True if the server closed the connection.
  bool closed_by_server() {
    char byte;
    return ::recv(_fd, &byte, 1, 0) == 0;
  }

private:
  bool read_exactly(void *buffer, size_t size) {
    size_t got = 0;
    while (got < size) {
      ssize_t n = ::recv(_fd, static_cast<char *>(buffer) + got, size - got, 0);
      if (n <= 0) {
        return false;
      }
      got += n;
    }
    return true;
  }

  int _fd;
  std::map<uint64_t, bool> _reads;
};

class ServerTest
    : public ::testing::TestWithParam<std::tuple<WioBackend, bool>> {
protected:
  void SetUp() override {
    char path[] = "/tmp/wheelie-test-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, file_size), 0);
    ::close(fd);
    _path = path;

    ServerConfig config;
    config.host = "127.0.0.1";
    config.file = _path;
    config.backend = std::get<0>(GetParam());
    config.multishot = std::get<1>(GetParam());
    config.threads = 2;
    config.queue_depth = 256;
    config.direct = false;
    _server = std::make_unique<Server>(config);
    try {
      _port = _server->start();
    } catch (const std::system_error &e) {
      if (e.code().value() == EPERM || e.code().value() == ENOSYS) {
        GTEST_SKIP() << wio_backend_name(config.backend)
                     << " is not available: " << e.what();
      }
      throw;
    }
  }

  void TearDown() override {
    _server.reset();
    ::unlink(_path.c_str());
  }

  std::string _path;
  std::unique_ptr<Server> _server;
  uint16_t _port = 0;
};

} // namespace

TEST_P(ServerTest, WriteThenRead) {
  Client client(_port);
  std::vector<char> data = pattern(4096, 1);

  client.send_request(1, WHEELIE_OP_WRITE, 8192, data.size(), data);
  Client::Response written = client.read_response();
  EXPECT_EQ(written.cid, 1u);
  EXPECT_EQ(written.res, 4096);
  EXPECT_TRUE(written.data.empty());

  client.send_request(2, WHEELIE_OP_READ, 8192, 4096);
  Client::Response read = client.read_response();
  EXPECT_EQ(read.cid, 2u);
  EXPECT_EQ(read.res, 4096);
  EXPECT_EQ(read.data, data);
}

TEST_P(ServerTest, DurableWriteThenRead) {
  Client client(_port);
  std::vector<char> data = pattern(8192, 2);

  client.send_request(1, WHEELIE_OP_WRITE_SYNC, 0, data.size(), data);
  EXPECT_EQ(client.read_response().res, 8192);

  client.send_request(2, WHEELIE_OP_READ, 0, 8192);
  EXPECT_EQ(client.read_response().data, data);
}

TEST_P(ServerTest, ReadPastTheEndIsShort) {
  Client client(_port);
  std::vector<char> data = pattern(2048, 3);
  client.send_request(1, WHEELIE_OP_WRITE, file_size - 2048, 2048, data);
  ASSERT_EQ(client.read_response().res, 2048);

  client.send_request(2, WHEELIE_OP_READ, file_size - 2048, 4096);
  Client::Response tail = client.read_response();
  EXPECT_EQ(tail.res, 2048);
  EXPECT_EQ(tail.data, data);

  client.send_request(3, WHEELIE_OP_READ, file_size, 4096);
  Client::Response past = client.read_response();
  EXPECT_EQ(past.res, 0);
  EXPECT_TRUE(past.data.empty());
}

TEST_P(ServerTest, PipelinedRequestsAreAllAnswered) {
  Client client(_port);
  constexpr int count = 16;
  std::vector<std::vector<char>> data;
  for (int i = 0; i < count; ++i) {
    data.push_back(pattern(4096, i));
    client.send_request(100 + i, WHEELIE_OP_WRITE, i * 4096, 4096, data[i]);
  }
  for (int i = 0; i < count; ++i) {
    client.send_request(200 + i, WHEELIE_OP_READ, i * 4096, 4096);
  }

  // The answers may come in any order; the reads may overtake the writes they
  // were sent after, so only the writes are checked here, and the contents
  // are checked again below.
  int writes = 0, reads = 0;
  for (int i = 0; i < 2 * count; ++i) {
    Client::Response response = client.read_response();
    ASSERT_GE(response.res, 0) << response.cid;
    if (response.cid >= 200) {
      ++reads;
    } else {
      EXPECT_EQ(response.res, 4096);
      ++writes;
    }
  }
  EXPECT_EQ(writes, count);
  EXPECT_EQ(reads, count);

  for (int i = 0; i < count; ++i) {
    client.send_request(300 + i, WHEELIE_OP_READ, i * 4096, 4096);
  }
  std::map<uint64_t, std::vector<char>> back;
  for (int i = 0; i < count; ++i) {
    Client::Response response = client.read_response();
    ASSERT_EQ(response.res, 4096);
    back[response.cid] = response.data;
  }
  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(back[300 + i], data[i]) << i;
  }
}

TEST_P(ServerTest, LargeWriteAndRead) {
  Client client(_port);
  std::vector<char> data = pattern(WHEELIE_MAX_PAYLOAD_SIZE, 5);

  client.send_request(1, WHEELIE_OP_WRITE, 4096, data.size(), data);
  ASSERT_EQ(client.read_response().res, static_cast<int64_t>(data.size()));

  client.send_request(2, WHEELIE_OP_READ, 4096, data.size());
  Client::Response read = client.read_response();
  ASSERT_EQ(read.res, static_cast<int64_t>(data.size()));
  EXPECT_TRUE(read.data == data);
}

TEST_P(ServerTest, OversizedReadIsRefusedAndTheConnectionLives) {
  Client client(_port);

  client.send_request(1, WHEELIE_OP_READ, 0, WHEELIE_MAX_PAYLOAD_SIZE + 1);
  Client::Response refused = client.read_response();
  EXPECT_EQ(refused.cid, 1u);
  EXPECT_EQ(refused.res, -EMSGSIZE);

  std::vector<char> data = pattern(4096, 6);
  client.send_request(2, WHEELIE_OP_WRITE, 0, data.size(), data);
  EXPECT_EQ(client.read_response().res, 4096);
}

TEST_P(ServerTest, UnknownOperationEndsTheConnection) {
  Client client(_port);
  client.send_request(1, 9, 0, 0);
  EXPECT_TRUE(client.closed_by_server());

  // The server carries on.
  Client other(_port);
  other.send_request(1, WHEELIE_OP_READ, 0, 4096);
  EXPECT_EQ(other.read_response().res, 4096);
}

TEST_P(ServerTest, MixedResponsesShareOneConnection) {
  Client client(_port);
  constexpr int blocks = 64;
  constexpr size_t big = 1024 * 1024;
  constexpr uint64_t big_offset = 4 * 1024 * 1024;

  std::vector<std::vector<char>> data;
  for (int i = 0; i < blocks; ++i) {
    data.push_back(pattern(4096, 20 + i));
    client.send_request(i, WHEELIE_OP_WRITE, i * 4096, 4096, data[i]);
  }
  std::vector<char> large = pattern(big, 99);
  client.send_request(1000, WHEELIE_OP_WRITE, big_offset, big, large);
  for (int i = 0; i <= blocks; ++i) {
    ASSERT_GE(client.read_response().res, 0);
  }

  // Many answers, of two very different sizes, are on their way back
  // interleaved on one stream: every one has to come out whole.
  constexpr int count = 200;
  std::map<uint64_t, const std::vector<char> *> expected;
  for (int i = 0; i < count; ++i) {
    uint64_t cid = 2000 + i;
    if (i % 10 == 0) {
      client.send_request(cid, WHEELIE_OP_READ, big_offset, big);
      expected[cid] = &large;
    } else {
      client.send_request(cid, WHEELIE_OP_READ, (i % blocks) * 4096, 4096);
      expected[cid] = &data[i % blocks];
    }
  }
  for (int i = 0; i < count; ++i) {
    Client::Response response = client.read_response();
    ASSERT_TRUE(expected.count(response.cid)) << response.cid;
    ASSERT_EQ(response.res,
              static_cast<int64_t>(expected[response.cid]->size()))
        << response.cid;
    ASSERT_TRUE(response.data == *expected[response.cid]) << response.cid;
    expected.erase(response.cid);
  }
  EXPECT_TRUE(expected.empty());
}

TEST_P(ServerTest, EveryAnswerIsSentBeforeTheConnectionCloses) {
  Client client(_port);
  std::vector<char> data = pattern(4096, 9);
  client.send_request(0, WHEELIE_OP_WRITE, 0, 4096, data);
  ASSERT_EQ(client.read_response().res, 4096);

  constexpr int count = 64;
  for (int i = 0; i < count; ++i) {
    client.send_request(100 + i, WHEELIE_OP_READ, 0, 4096);
  }
  client.finish_sending();

  // The server sees the end of the requests while it is still answering them.
  for (int i = 0; i < count; ++i) {
    Client::Response response = client.read_response();
    ASSERT_EQ(response.res, 4096) << i;
    EXPECT_EQ(response.data, data) << i;
  }
  EXPECT_TRUE(client.closed_by_server());
}

TEST_P(ServerTest, ClientMayLeaveWithRequestsInFlight) {
  {
    Client client(_port);
    std::vector<char> data = pattern(4096, 8);
    for (int i = 0; i < 32; ++i) {
      if (i % 2 == 0) {
        client.send_request(i, WHEELIE_OP_WRITE, i * 4096, 4096, data);
      } else {
        client.send_request(i, WHEELIE_OP_READ, i * 4096, 4096);
      }
    }
    // It leaves without reading an answer; its requests are still being
    // served when the server finds its connection closed.
  }

  Client other(_port);
  other.send_request(1, WHEELIE_OP_READ, 0, 4096);
  EXPECT_EQ(other.read_response().res, 4096);
}

TEST_P(ServerTest, ConnectionsDoNotGetInEachOthersWay) {
  constexpr int count = 6;
  std::vector<std::unique_ptr<Client>> clients;
  for (int i = 0; i < count; ++i) {
    clients.push_back(std::make_unique<Client>(_port));
  }
  std::vector<std::vector<char>> data;
  for (int i = 0; i < count; ++i) {
    data.push_back(pattern(4096, 10 + i));
    clients[i]->send_request(1, WHEELIE_OP_WRITE, i * 4096, 4096, data[i]);
  }
  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(clients[i]->read_response().res, 4096) << i;
  }
  for (int i = 0; i < count; ++i) {
    clients[i]->send_request(2, WHEELIE_OP_READ, i * 4096, 4096);
  }
  for (int i = count - 1; i >= 0; --i) {
    EXPECT_EQ(clients[i]->read_response().data, data[i]) << i;
  }
}

TEST_P(ServerTest, StopsWhileClientsAreConnected) {
  Client idle(_port);
  Client busy(_port);
  std::vector<char> data = pattern(4096, 7);
  busy.send_request(1, WHEELIE_OP_WRITE, 0, data.size(), data);
  ASSERT_EQ(busy.read_response().res, 4096);

  auto started = std::chrono::steady_clock::now();
  _server->stop();
  auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::seconds(3));

  EXPECT_TRUE(idle.closed_by_server());
}

INSTANTIATE_TEST_SUITE_P(
    Backends, ServerTest,
    ::testing::Combine(::testing::Values(WIO_BACKEND_LIBC, WIO_BACKEND_ALIBC,
                                         WIO_BACKEND_URING),
                       ::testing::Bool()),
    [](const ::testing::TestParamInfo<std::tuple<WioBackend, bool>> &info) {
      std::string name = wio_backend_name(std::get<0>(info.param));
      std::replace(name.begin(), name.end(), '-', '_');
      return name + (std::get<1>(info.param) ? "_multishot" : "_single");
    });

TEST(ServerDirectIo, MisalignedRequestsFailWithEinval) {
  char path[] = "./wheelie-test-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(ftruncate(fd, 1 << 20), 0);
  ::close(fd);

  ServerConfig config;
  config.host = "127.0.0.1";
  config.file = path;
  config.backend = WIO_BACKEND_URING;
  config.direct = true;
  Server server(config);
  uint16_t port = 0;
  try {
    port = server.start();
  } catch (const std::system_error &e) {
    ::unlink(path);
    GTEST_SKIP() << "no O_DIRECT here: " << e.what();
  }

  Client client(port);
  std::vector<char> aligned = pattern(4096, 1);
  client.send_request(1, WHEELIE_OP_WRITE, 4096, 4096, aligned);
  EXPECT_EQ(client.read_response().res, 4096);

  std::vector<char> odd = pattern(100, 2);
  client.send_request(2, WHEELIE_OP_WRITE, 4096, 100, odd);
  EXPECT_EQ(client.read_response().res, -EINVAL);

  client.send_request(3, WHEELIE_OP_READ, 4096, 4096);
  EXPECT_EQ(client.read_response().data, aligned);
  ::unlink(path);
}
