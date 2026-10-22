#include <client/client.hpp>

#include <server/server.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

using wheelie::Client;
using wheelie::ClientOptions;
using wheelie::Server;
using wheelie::ServerConfig;
using wheelio::Task;

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

Task<std::vector<char>> write_then_read(Client &client, uint64_t offset,
                                        std::vector<char> data,
                                        unsigned int flags) {
  size_t written =
      co_await client.pwrite(data.data(), data.size(), offset, flags);
  if (written != data.size()) {
    throw std::runtime_error("short write");
  }
  std::vector<char> back(data.size());
  size_t read = co_await client.pread(back.data(), back.size(), offset);
  back.resize(read);
  co_return back;
}

// Starts a read and a write and awaits them in the other order.
Task<int> reversed(Client &client, char *read_buffer,
                   const char *write_buffer) {
  auto read = client.pread(read_buffer, 4096, 0);
  auto write = client.pwrite(write_buffer, 4096, 4096);
  size_t written = co_await write;
  size_t got = co_await read;
  co_return static_cast<int>(written + got);
}

Task<int> errno_of_read(Client &client, void *buffer, size_t size,
                        uint64_t offset) {
  try {
    co_await client.pread(buffer, size, offset);
  } catch (const std::system_error &e) {
    co_return e.code().value();
  }
  co_return 0;
}

class ClientCoroTest
    : public ::testing::TestWithParam<std::tuple<WioBackend, bool>> {
protected:
  void SetUp() override {
    char path[] = "/tmp/wheelie-client-coro-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, file_size), 0);
    ::close(fd);
    _path = path;

    ServerConfig config;
    config.host = "127.0.0.1";
    config.file = _path;
    config.backend = WIO_BACKEND_LIBC;
    config.threads = 2;
    config.direct = false;
    _server = std::make_unique<Server>(config);
    _port = _server->start();

    try {
      _client = std::make_unique<Client>(options(depth));
    } catch (const std::system_error &e) {
      if (e.code().value() == EPERM || e.code().value() == ENOSYS) {
        GTEST_SKIP() << wio_backend_name(std::get<0>(GetParam()))
                     << " is not available: " << e.what();
      }
      throw;
    }
  }

  void TearDown() override {
    _client.reset();
    _server.reset();
    ::unlink(_path.c_str());
  }

  ClientOptions options(unsigned int client_depth) const {
    ClientOptions options;
    options.host = "127.0.0.1";
    options.port = _port;
    options.backend = std::get<0>(GetParam());
    options.multishot = std::get<1>(GetParam());
    options.depth = client_depth;
    return options;
  }

  // Runs the client's reactor until `task` is done.
  template <typename T> bool run(Task<T> &task) {
    for (int i = 0; i < 10000 && !task.done(); ++i) {
      _client->reactor().wait(1000);
    }
    return task.done();
  }

  std::string _path;
  std::unique_ptr<Server> _server;
  uint16_t _port = 0;
  std::unique_ptr<Client> _client;
};

} // namespace

TEST_P(ClientCoroTest, WriteThenReadWithCoAwait) {
  std::vector<char> data = pattern(4096, 1);
  Task<std::vector<char>> task = write_then_read(*_client, 8192, data, 0);
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), data);
}

TEST_P(ClientCoroTest, DurableWriteWithCoAwait) {
  std::vector<char> data = pattern(8192, 2);
  Task<std::vector<char>> task =
      write_then_read(*_client, 0, data, WHEELIE_FLAG_SYNC);
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), data);
}

TEST_P(ClientCoroTest, RequestMayFinishBeforeItIsAwaited) {
  std::vector<char> read_buffer(4096), write_buffer = pattern(4096, 3);
  Task<int> task = reversed(*_client, read_buffer.data(), write_buffer.data());
  EXPECT_EQ(_client->inflight(), 2u);
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), 8192);
  EXPECT_EQ(_client->inflight(), 0u);
}

TEST_P(ClientCoroTest, ManyCoroutinesShareTheConnection) {
  std::vector<std::vector<char>> data;
  std::vector<Task<std::vector<char>>> tasks;
  for (unsigned int i = 0; i < depth / 2; ++i) {
    data.push_back(pattern(4096, 10 + i));
    tasks.push_back(write_then_read(*_client, i * 4096, data[i], 0));
  }
  for (unsigned int i = 0; i < tasks.size(); ++i) {
    ASSERT_TRUE(run(tasks[i])) << i;
    EXPECT_EQ(tasks[i].get(), data[i]) << i;
  }
}

TEST_P(ClientCoroTest, ARequestThatCannotBeSubmittedThrowsFromTheCall) {
  _client = std::make_unique<Client>(options(2));
  std::vector<char> data(4096), big(WHEELIE_MAX_PAYLOAD_SIZE + 1);

  auto first = _client->pread(data.data(), data.size(), 0);
  auto second = _client->pread(data.data(), data.size(), 0);
  try {
    _client->pread(data.data(), data.size(), 0);
    ADD_FAILURE() << "no exception";
  } catch (const std::system_error &e) {
    EXPECT_EQ(e.code().value(), EBUSY);
  }

  // Awaiting what was submitted lets the client finish.
  struct Waiter {
    static Task<void> wait(Client::Request &request) { co_await request; }
  };
  Task<void> first_done = Waiter::wait(first);
  Task<void> second_done = Waiter::wait(second);
  ASSERT_TRUE(run(first_done));
  ASSERT_TRUE(run(second_done));

  EXPECT_THROW(_client->pread(big.data(), big.size(), 0), std::system_error);
  EXPECT_THROW(_client->pwrite(data.data(), data.size(), 0, 0x80),
               std::system_error);
}

TEST_P(ClientCoroTest, BrokenConnectionFailsWhatWaitsAndWhatComesAfter) {
  std::vector<char> buffer(4096);
  Task<int> task = errno_of_read(*_client, buffer.data(), buffer.size(), 0);

  _server->stop();
  ASSERT_TRUE(run(task));
  int error = task.get();
  EXPECT_NE(error, 0);
  // What broke the connection, not the cancellation the client uses itself.
  EXPECT_NE(error, ECANCELED);

  EXPECT_THROW(_client->pread(buffer.data(), buffer.size(), 0),
               std::system_error);
}

TEST_P(ClientCoroTest, DeletingTheClientResumesWhatWaitsWithEcanceled) {
  std::vector<char> buffer(4096);
  Task<int> task = errno_of_read(*_client, buffer.data(), buffer.size(), 0);
  EXPECT_FALSE(task.done());

  _client.reset();
  ASSERT_TRUE(task.done());
  EXPECT_EQ(task.get(), ECANCELED);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, ClientCoroTest,
    ::testing::Combine(::testing::Values(WIO_BACKEND_LIBC, WIO_BACKEND_ALIBC,
                                         WIO_BACKEND_URING),
                       ::testing::Bool()),
    [](const ::testing::TestParamInfo<std::tuple<WioBackend, bool>> &info) {
      return std::string(wio_backend_name(std::get<0>(info.param))) +
             (std::get<1>(info.param) ? "_multishot" : "_single");
    });
