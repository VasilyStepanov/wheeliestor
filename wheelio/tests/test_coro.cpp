#include "fd.hpp"

#include <wheelio/reactor.hpp>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using wheelio::Operation;
using wheelio::Reactor;
using wheelio::RecvStream;
using wheelio::Task;

namespace {

Task<int> forty_one() { co_return 41; }

Task<int> forty_two() {
  int value = co_await forty_one();
  co_return value + 1;
}

Task<void> throws() {
  throw std::runtime_error("boom");
  co_return;
}

Task<std::string> catches() {
  try {
    co_await throws();
  } catch (const std::runtime_error &e) {
    co_return e.what();
  }
  co_return "";
}

} // namespace

TEST(Task, RunsAtOnceAndChains) {
  Task<int> task = forty_two();
  EXPECT_TRUE(task.done());
  EXPECT_EQ(task.get(), 42);
}

TEST(Task, ExceptionTravelsThroughAwait) {
  Task<std::string> task = catches();
  ASSERT_TRUE(task.done());
  EXPECT_EQ(task.get(), "boom");
}

TEST(Task, ExceptionIsKeptUntilGet) {
  Task<void> task = throws();
  EXPECT_TRUE(task.done());
  EXPECT_THROW(task.get(), std::runtime_error);
}

namespace {

Task<void> echo_once(Reactor &reactor, int fd) {
  char buf[64];
  size_t received = co_await reactor.recv(fd, buf, sizeof(buf));
  size_t sent = co_await reactor.send(fd, buf, received);
  if (sent != received) {
    throw std::runtime_error("short send");
  }
}

Task<std::string> ask(Reactor &reactor, int fd, std::string message) {
  co_await reactor.send(fd, message.data(), message.size());
  char buf[64];
  size_t received = co_await reactor.recv(fd, buf, sizeof(buf));
  co_return std::string(buf, received);
}

// Starts two receives and awaits them in the other order.
Task<int> reversed(Reactor &reactor, int first_fd, int second_fd) {
  char first_buf[8], second_buf[8];
  auto first = reactor.recv(first_fd, first_buf, sizeof(first_buf));
  auto second = reactor.recv(second_fd, second_buf, sizeof(second_buf));
  size_t second_size = co_await second;
  size_t first_size = co_await first;
  co_return static_cast<int>(first_size * 100 + second_size);
}

Task<int> errno_of_bad_recv(Reactor &reactor) {
  char buf[8];
  try {
    co_await reactor.recv(-1, buf, sizeof(buf));
  } catch (const std::system_error &e) {
    co_return e.code().value();
  }
  co_return 0;
}

Task<int> errno_of(Operation<size_t> &operation) {
  try {
    co_await operation;
  } catch (const std::system_error &e) {
    co_return e.code().value();
  }
  co_return 0;
}

Task<std::string> drain(Reactor &reactor, int fd) {
  RecvStream stream = reactor.recv_multishot(fd, 16, 4);
  std::string all;
  while (auto chunk = co_await stream.next()) {
    all.append(reinterpret_cast<const char *>(chunk->data()), chunk->size());
  }
  co_return all;
}

// Waits for something else first, while the stream's chunks pile up.
Task<std::string> busy_then_drain(Reactor &reactor, int stream_fd,
                                  int other_fd) {
  RecvStream stream = reactor.recv_multishot(stream_fd, 16, 4);
  char buf[4];
  co_await reactor.recv(other_fd, buf, sizeof(buf));
  std::string all;
  while (auto chunk = co_await stream.next()) {
    all.append(reinterpret_cast<const char *>(chunk->data()), chunk->size());
  }
  co_return all;
}

Task<int> errno_of_stream(RecvStream &stream) {
  try {
    while (co_await stream.next()) {
    }
  } catch (const std::system_error &e) {
    co_return e.code().value();
  }
  co_return 0;
}

Task<void> fail_after_recv(Reactor &reactor, int fd) {
  char buf[8];
  co_await reactor.recv(fd, buf, sizeof(buf));
  throw std::runtime_error("from a spawned task");
}

class CoroTest : public ::testing::TestWithParam<WioBackend> {
protected:
  void SetUp() override {
    try {
      reactor_.emplace(GetParam(), 128);
    } catch (const std::system_error &e) {
      if (e.code().value() == EPERM || e.code().value() == ENOSYS) {
        GTEST_SKIP() << wio_backend_name(GetParam())
                     << " is not available: " << e.what();
      }
      throw;
    }
  }

  // Runs the reactor until `task` is done.
  template <typename T> bool run(Task<T> &task) {
    for (int i = 0; i < 100000 && !task.done(); ++i) {
      reactor_->wait(1000);
    }
    return task.done();
  }

  // Runs the reactor until nothing is in flight.
  bool drain_reactor() {
    for (int i = 0; i < 100000 && reactor_->inflight() > 0; ++i) {
      reactor_->wait(1000);
    }
    return reactor_->inflight() == 0;
  }

  std::optional<Reactor> reactor_;
};

} // namespace

TEST_P(CoroTest, EchoOverSocketpair) {
  Fd client, server;
  ASSERT_TRUE(socket_pair(client, server));

  Task<void> echo = echo_once(*reactor_, server.get());
  Task<std::string> reply = ask(*reactor_, client.get(), "hello");
  ASSERT_TRUE(run(reply));
  EXPECT_EQ(reply.get(), "hello");
  EXPECT_TRUE(echo.done());
  echo.get();
}

TEST_P(CoroTest, OperationMayCompleteBeforeItIsAwaited) {
  Fd a, b, c, d;
  ASSERT_TRUE(socket_pair(a, b));
  ASSERT_TRUE(socket_pair(c, d));
  ASSERT_EQ(::write(b.get(), "xyz", 3), 3);
  ASSERT_EQ(::write(d.get(), "12", 2), 2);

  Task<int> task = reversed(*reactor_, a.get(), c.get());
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), 302);
}

TEST_P(CoroTest, FailedOperationThrowsSystemError) {
  Task<int> task = errno_of_bad_recv(*reactor_);
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), EBADF);
}

TEST_P(CoroTest, FailedSubmissionThrowsFromTheCall) {
  Reactor small(GetParam(), 1);
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  char buf[8];

  auto first = small.recv(b.get(), buf, sizeof(buf));
  EXPECT_THROW(small.recv(b.get(), buf, sizeof(buf)), std::system_error);

  Task<int> waiting = errno_of(first);
  ASSERT_TRUE(first.cancel());
  for (int i = 0; i < 1000 && !waiting.done(); ++i) {
    small.wait(1000);
  }
  ASSERT_TRUE(waiting.done());
  EXPECT_EQ(waiting.get(), ECANCELED);
}

TEST_P(CoroTest, CancelledOperationThrowsEcanceled) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  char buf[8];

  auto operation = reactor_->recv(b.get(), buf, sizeof(buf));
  Task<int> task = errno_of(operation);
  EXPECT_TRUE(operation.cancel());
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), ECANCELED);
  EXPECT_FALSE(operation.cancel());
}

TEST_P(CoroTest, SpawnedTaskThatThrowsSurfacesInWait) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));

  wheelio::spawn(fail_after_recv(*reactor_, b.get()));
  ASSERT_EQ(::write(a.get(), "go", 2), 2);

  bool thrown = false;
  try {
    for (int i = 0; i < 1000; ++i) {
      reactor_->wait(1000);
    }
  } catch (const std::runtime_error &e) {
    thrown = std::string(e.what()) == "from a spawned task";
  }
  EXPECT_TRUE(thrown);
}

TEST_P(CoroTest, RecvStreamDeliversEverythingThenEnds) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  std::string sent;
  for (int i = 0; i < 100; ++i) {
    sent += static_cast<char>('A' + i % 26);
  }
  ASSERT_EQ(::write(a.get(), sent.data(), sent.size()), 100);
  a.reset();

  Task<std::string> task = drain(*reactor_, b.get());
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), sent);
  EXPECT_EQ(reactor_->inflight(), 0u);
}

TEST_P(CoroTest, RecvStreamKeepsChunksForABusyConsumer) {
  Fd a, b, c, d;
  ASSERT_TRUE(socket_pair(a, b));
  ASSERT_TRUE(socket_pair(c, d));
  std::string sent(50, 'q');

  Task<std::string> task = busy_then_drain(*reactor_, b.get(), c.get());
  ASSERT_EQ(::write(a.get(), sent.data(), sent.size()), 50);
  a.reset();
  // The stream has run to its end while the task waits for the other socket.
  for (int i = 0; i < 1000 && reactor_->inflight() > 1; ++i) {
    reactor_->wait(1000);
  }
  ASSERT_EQ(reactor_->inflight(), 1u);
  EXPECT_FALSE(task.done());

  ASSERT_EQ(::write(d.get(), "wake", 4), 4);
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), sent);
}

TEST_P(CoroTest, RecvStreamCancelThrowsEcanceled) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));

  RecvStream stream = reactor_->recv_multishot(b.get(), 16, 4);
  Task<int> task = errno_of_stream(stream);
  EXPECT_TRUE(stream.cancel());
  ASSERT_TRUE(run(task));
  EXPECT_EQ(task.get(), ECANCELED);
  EXPECT_FALSE(stream.cancel());
}

TEST_P(CoroTest, RecvStreamDestroyedWhileReceivingCleansUpAfterItself) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  {
    RecvStream stream = reactor_->recv_multishot(b.get(), 16, 4);
    EXPECT_EQ(reactor_->inflight(), 1u);
  }
  ASSERT_TRUE(drain_reactor());
}

TEST_P(CoroTest, ManyEchoesAtOnce) {
  constexpr int pairs = 32;
  std::vector<Fd> clients(pairs), servers(pairs);
  std::vector<Task<void>> echoes;
  std::vector<Task<std::string>> replies;
  for (int i = 0; i < pairs; ++i) {
    ASSERT_TRUE(socket_pair(clients[i], servers[i]));
    echoes.push_back(echo_once(*reactor_, servers[i].get()));
    replies.push_back(
        ask(*reactor_, clients[i].get(), "message " + std::to_string(i)));
  }
  for (int i = 0; i < pairs; ++i) {
    ASSERT_TRUE(run(replies[i])) << i;
    EXPECT_EQ(replies[i].get(), "message " + std::to_string(i));
    EXPECT_TRUE(echoes[i].done());
  }
}

INSTANTIATE_TEST_SUITE_P(Backends, CoroTest,
                         ::testing::Values(WIO_BACKEND_LIBC, WIO_BACKEND_ALIBC,
                                           WIO_BACKEND_URING),
                         [](const ::testing::TestParamInfo<WioBackend> &info) {
                           return std::string(wio_backend_name(info.param));
                         });
