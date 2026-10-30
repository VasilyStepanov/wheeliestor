#include "fd.hpp"

#include <wheelio/wheelio.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace {

struct Request {
  WioOp op;
  int calls;
  int result;
  unsigned int flags;
};

void on_done(WioOp *op, int result, unsigned int flags) {
  Request *request = wio_container_of(op, Request, op);
  ++request->calls;
  request->result = result;
  request->flags = flags;
}

struct StreamRequest {
  WioOp op;
  char data[8192];
  size_t size;
  size_t max_chunk;
  int calls;
  int more_calls;
  bool finished;
  int final_result;
  unsigned int final_flags;
};

void on_chunk(WioOp *op, const void *data, int result, unsigned int flags) {
  StreamRequest *request = wio_container_of(op, StreamRequest, op);
  ++request->calls;
  if (flags & WIO_F_MORE) {
    ++request->more_calls;
  }
  if (result > 0) {
    if (request->size + result <= sizeof(request->data)) {
      memcpy(request->data + request->size, data, result);
    }
    request->size += result;
    if (static_cast<size_t>(result) > request->max_chunk) {
      request->max_chunk = result;
    }
  }
  if (!(flags & WIO_F_MORE)) {
    request->finished = true;
    request->final_result = result;
    request->final_flags = flags;
  }
}

class ReactorTest : public ::testing::TestWithParam<WioBackend> {
protected:
  void SetUp() override {
    int ret = wio_reactor_create(GetParam(), 16, &reactor_);
    if (ret == -EPERM || ret == -ENOSYS) {
      GTEST_SKIP() << wio_backend_name(GetParam())
                   << " is not available: " << strerror(-ret);
    }
    ASSERT_EQ(ret, 0);
  }

  void TearDown() override { wio_reactor_delete(reactor_); }

  // Runs the reactor until `done` holds.
  bool run_until(const std::function<bool()> &done) {
    for (int i = 0; i < 100000 && !done(); ++i) {
      if (wio_wait(reactor_, 1000) < 0) {
        return false;
      }
    }
    return done();
  }

  // Runs the reactor until nothing is in flight any more.
  bool drain() {
    return run_until([this] { return wio_reactor_inflight(reactor_) == 0; });
  }

  WioReactor *reactor_ = nullptr;
};

} // namespace

TEST(Reactor, CreateRejectsInvalidDepth) {
  WioReactor *reactor = nullptr;
  EXPECT_EQ(wio_reactor_create(WIO_BACKEND_URING, 0, &reactor), -EINVAL);
  EXPECT_EQ(wio_reactor_create(WIO_BACKEND_URING, 1u << 20, &reactor), -EINVAL);
}

TEST(Reactor, CreateRejectsEmptyPool) {
  WioReactor *reactor = nullptr;
  EXPECT_EQ(wio_reactor_create_pool(WIO_BACKEND_ALIBC, 16, 0, &reactor),
            -EINVAL);
}

TEST(Reactor, CreateTakesPoolSize) {
  for (unsigned int threads : {1u, 8u}) {
    WioReactor *reactor = nullptr;
    ASSERT_EQ(wio_reactor_create_pool(WIO_BACKEND_ALIBC, 16, threads, &reactor),
              0);
    wio_reactor_delete(reactor);
  }
}

TEST_P(ReactorTest, SendAndRecv) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  const char out[] = "hello";
  char in[16] = {};
  Request send = {}, recv = {};

  ASSERT_EQ(wio_recv(reactor_, &recv.op, b.get(), in, sizeof(in), 0, on_done),
            0);
  ASSERT_EQ(wio_send(reactor_, &send.op, a.get(), out, 5, 0, on_done), 0);
  EXPECT_EQ(wio_reactor_inflight(reactor_), 2u);
  ASSERT_TRUE(run_until([&] { return send.calls == 1 && recv.calls == 1; }));

  EXPECT_EQ(send.result, 5);
  EXPECT_EQ(recv.result, 5);
  EXPECT_EQ(send.flags, 0u);
  EXPECT_EQ(recv.flags, 0u);
  EXPECT_EQ(memcmp(in, out, 5), 0);
  EXPECT_EQ(wio_reactor_inflight(reactor_), 0u);
}

TEST_P(ReactorTest, SendmsgGathersBuffers) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  char head[] = "head-";
  char body[] = "body";
  iovec iov[2] = {{head, 5}, {body, 4}};
  msghdr msg = {};
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  char in[16] = {};
  Request send = {}, recv = {};

  ASSERT_EQ(wio_sendmsg(reactor_, &send.op, a.get(), &msg, 0, on_done), 0);
  ASSERT_TRUE(run_until([&] { return send.calls == 1; }));
  EXPECT_EQ(send.result, 9);

  ASSERT_EQ(wio_recv(reactor_, &recv.op, b.get(), in, sizeof(in), 0, on_done),
            0);
  ASSERT_TRUE(run_until([&] { return recv.calls == 1; }));
  EXPECT_EQ(recv.result, 9);
  EXPECT_EQ(memcmp(in, "head-body", 9), 0);
}

TEST_P(ReactorTest, RecvSeesEndOfStream) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  char in[16];
  Request recv = {};

  ASSERT_EQ(wio_recv(reactor_, &recv.op, b.get(), in, sizeof(in), 0, on_done),
            0);
  a.reset();
  ASSERT_TRUE(run_until([&] { return recv.calls == 1; }));
  EXPECT_EQ(recv.result, 0);
}

TEST_P(ReactorTest, SendToClosedPeerFailsWithoutSignal) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  b.reset();
  Request send = {};

  ASSERT_EQ(wio_send(reactor_, &send.op, a.get(), "x", 1, 0, on_done), 0);
  ASSERT_TRUE(run_until([&] { return send.calls == 1; }));
  EXPECT_EQ(send.result, -EPIPE);
}

TEST_P(ReactorTest, WaitReturnsZeroWhenNothingIsInFlight) {
  EXPECT_EQ(wio_wait(reactor_, -1), 0);
  EXPECT_EQ(wio_wait(reactor_, 0), 0);
  EXPECT_EQ(wio_wait(reactor_, 10), 0);
}

TEST_P(ReactorTest, WaitTimesOutAndReportsCallbacks) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  char in[16];
  Request recv = {};

  ASSERT_EQ(wio_recv(reactor_, &recv.op, b.get(), in, sizeof(in), 0, on_done),
            0);
  EXPECT_EQ(wio_wait(reactor_, 20), 0);
  EXPECT_EQ(recv.calls, 0);
  EXPECT_EQ(wio_reactor_inflight(reactor_), 1u);

  ASSERT_EQ(::write(a.get(), "z", 1), 1);
  EXPECT_EQ(wio_wait(reactor_, 1000), 1);
  EXPECT_EQ(recv.calls, 1);
}

TEST_P(ReactorTest, SubmitBeyondDepthIsBusy) {
  WioReactor *small = nullptr;
  ASSERT_EQ(wio_reactor_create(GetParam(), 2, &small), 0);
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  char in[3][8];
  Request recv[3] = {};

  EXPECT_EQ(wio_recv(small, &recv[0].op, b.get(), in[0], 8, 0, on_done), 0);
  EXPECT_EQ(wio_recv(small, &recv[1].op, b.get(), in[1], 8, 0, on_done), 0);
  EXPECT_EQ(wio_recv(small, &recv[2].op, b.get(), in[2], 8, 0, on_done),
            -EBUSY);
  EXPECT_EQ(wio_reactor_inflight(small), 2u);

  ASSERT_EQ(wio_cancel_fd(small, b.get()), 0);
  while (wio_reactor_inflight(small) > 0) {
    ASSERT_GE(wio_wait(small, 1000), 0);
  }
  EXPECT_EQ(recv[0].result, -ECANCELED);
  EXPECT_EQ(recv[1].result, -ECANCELED);
  EXPECT_EQ(recv[2].calls, 0);
  wio_reactor_delete(small);
}

TEST_P(ReactorTest, CancelCompletesWithCanceled) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  char in[16];
  Request recv = {};

  ASSERT_EQ(wio_recv(reactor_, &recv.op, b.get(), in, sizeof(in), 0, on_done),
            0);
  ASSERT_EQ(wio_cancel(reactor_, &recv.op), 0);
  ASSERT_TRUE(drain());
  EXPECT_EQ(recv.calls, 1);
  EXPECT_EQ(recv.result, -ECANCELED);
}

TEST_P(ReactorTest, CancelFdCancelsEveryOperationOnIt) {
  Fd a, b, c, d;
  ASSERT_TRUE(socket_pair(a, b));
  ASSERT_TRUE(socket_pair(c, d));
  char in[3][8];
  Request on_b[2] = {}, on_d = {};

  ASSERT_EQ(wio_recv(reactor_, &on_b[0].op, b.get(), in[0], 8, 0, on_done), 0);
  ASSERT_EQ(wio_recv(reactor_, &on_b[1].op, b.get(), in[1], 8, 0, on_done), 0);
  ASSERT_EQ(wio_recv(reactor_, &on_d.op, d.get(), in[2], 8, 0, on_done), 0);
  ASSERT_EQ(wio_cancel_fd(reactor_, b.get()), 0);
  ASSERT_TRUE(run_until([&] { return on_b[0].calls && on_b[1].calls; }));

  EXPECT_EQ(on_b[0].result, -ECANCELED);
  EXPECT_EQ(on_b[1].result, -ECANCELED);
  EXPECT_EQ(on_d.calls, 0);
  EXPECT_EQ(wio_reactor_inflight(reactor_), 1u);

  ASSERT_EQ(wio_cancel_fd(reactor_, d.get()), 0);
  ASSERT_TRUE(drain());
}

TEST_P(ReactorTest, CloseClosesTheDescriptor) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  Fd write_end(fds[1]);
  Request close = {};

  ASSERT_EQ(wio_close(reactor_, &close.op, fds[0], on_done), 0);
  ASSERT_TRUE(run_until([&] { return close.calls == 1; }));
  EXPECT_EQ(close.result, 0);

  char c;
  EXPECT_EQ(::read(fds[0], &c, 1), -1);
  EXPECT_EQ(errno, EBADF);
}

TEST_P(ReactorTest, PwritePreadFsync) {
  char path[] = "/tmp/wheelio-test-XXXXXX";
  Fd file(mkstemp(path));
  ASSERT_GE(file.get(), 0);
  ASSERT_EQ(unlink(path), 0);

  char out[8192];
  for (size_t i = 0; i < sizeof(out); ++i) {
    out[i] = static_cast<char>(i * 7);
  }
  Request write = {}, dsync = {}, sync = {}, read = {};

  ASSERT_EQ(
      wio_pwrite(reactor_, &write.op, file.get(), out, 4096, 0, 0, on_done), 0);
  ASSERT_TRUE(run_until([&] { return write.calls == 1; }));
  EXPECT_EQ(write.result, 4096);

  ASSERT_EQ(wio_pwrite(reactor_, &dsync.op, file.get(), out + 4096, 4096, 4096,
                       WIO_WRITE_DSYNC, on_done),
            0);
  ASSERT_TRUE(run_until([&] { return dsync.calls == 1; }));
  EXPECT_EQ(dsync.result, 4096);

  ASSERT_EQ(wio_fsync(reactor_, &sync.op, file.get(), false, on_done), 0);
  ASSERT_TRUE(run_until([&] { return sync.calls == 1; }));
  EXPECT_EQ(sync.result, 0);

  char in[8192] = {};
  ASSERT_EQ(
      wio_pread(reactor_, &read.op, file.get(), in, sizeof(in), 0, on_done), 0);
  ASSERT_TRUE(run_until([&] { return read.calls == 1; }));
  EXPECT_EQ(read.result, 8192);
  EXPECT_EQ(memcmp(in, out, sizeof(out)), 0);
}

TEST_P(ReactorTest, ManyDiskOperationsAtOnce) {
  WioReactor *wide = nullptr;
  ASSERT_EQ(wio_reactor_create(GetParam(), 64, &wide), 0);
  char path[] = "/tmp/wheelio-test-XXXXXX";
  Fd file(mkstemp(path));
  ASSERT_GE(file.get(), 0);
  ASSERT_EQ(unlink(path), 0);

  constexpr int count = 64;
  constexpr size_t block = 4096;
  static char out[count][block];
  static char in[count][block];
  Request write[count] = {}, read[count] = {};

  for (int i = 0; i < count; ++i) {
    memset(out[i], 'a' + i % 26, block);
    ASSERT_EQ(wio_pwrite(wide, &write[i].op, file.get(), out[i], block,
                         i * block, i % 2 ? WIO_WRITE_DSYNC : 0, on_done),
              0);
  }
  while (wio_reactor_inflight(wide) > 0) {
    ASSERT_GE(wio_wait(wide, 1000), 0);
  }
  for (int i = 0; i < count; ++i) {
    ASSERT_EQ(write[i].calls, 1) << i;
    ASSERT_EQ(write[i].result, static_cast<int>(block)) << i;
  }

  for (int i = 0; i < count; ++i) {
    ASSERT_EQ(wio_pread(wide, &read[i].op, file.get(), in[i], block, i * block,
                        on_done),
              0);
  }
  while (wio_reactor_inflight(wide) > 0) {
    ASSERT_GE(wio_wait(wide, 1000), 0);
  }
  for (int i = 0; i < count; ++i) {
    ASSERT_EQ(read[i].result, static_cast<int>(block)) << i;
    ASSERT_EQ(memcmp(in[i], out[i], block), 0) << i;
  }
  wio_reactor_delete(wide);
}

TEST_P(ReactorTest, AcceptAndConnect) {
  Fd listener(socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_GE(listener.get(), 0);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(
      bind(listener.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)),
      0);
  ASSERT_EQ(listen(listener.get(), 4), 0);
  socklen_t addr_len = sizeof(addr);
  ASSERT_EQ(getsockname(listener.get(), reinterpret_cast<sockaddr *>(&addr),
                        &addr_len),
            0);

  Fd client(socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_GE(client.get(), 0);
  sockaddr_in peer = {};
  socklen_t peer_len = sizeof(peer);
  Request accept = {}, connect = {};

  ASSERT_EQ(wio_accept(reactor_, &accept.op, listener.get(),
                       reinterpret_cast<sockaddr *>(&peer), &peer_len, on_done),
            0);
  ASSERT_EQ(wio_connect(reactor_, &connect.op, client.get(),
                        reinterpret_cast<sockaddr *>(&addr), sizeof(addr),
                        on_done),
            0);
  ASSERT_TRUE(
      run_until([&] { return accept.calls == 1 && connect.calls == 1; }));

  EXPECT_EQ(connect.result, 0);
  ASSERT_GE(accept.result, 0);
  Fd server(accept.result);
  EXPECT_EQ(peer.sin_family, AF_INET);
  EXPECT_EQ(peer.sin_addr.s_addr, htonl(INADDR_LOOPBACK));

  ASSERT_EQ(::write(client.get(), "ping", 4), 4);
  char in[4];
  ASSERT_EQ(::read(server.get(), in, sizeof(in)), 4);
  EXPECT_EQ(memcmp(in, "ping", 4), 0);
}

TEST_P(ReactorTest, RecvMultishotRejectsBadRing) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  StreamRequest stream = {};

  EXPECT_EQ(
      wio_recv_multishot(reactor_, &stream.op, b.get(), 0, 4, 0, on_chunk),
      -EINVAL);
  EXPECT_EQ(
      wio_recv_multishot(reactor_, &stream.op, b.get(), 16, 0, 0, on_chunk),
      -EINVAL);
  EXPECT_EQ(
      wio_recv_multishot(reactor_, &stream.op, b.get(), 16, 3, 0, on_chunk),
      -EINVAL);
  EXPECT_EQ(wio_reactor_inflight(reactor_), 0u);
}

TEST_P(ReactorTest, RecvMultishotDeliversChunksUntilCancelled) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  StreamRequest stream = {};

  ASSERT_EQ(
      wio_recv_multishot(reactor_, &stream.op, b.get(), 16, 4, 0, on_chunk), 0);
  ASSERT_EQ(::write(a.get(), "hello", 5), 5);
  ASSERT_TRUE(run_until([&] { return stream.size == 5; }));
  EXPECT_EQ(wio_reactor_inflight(reactor_), 1u);

  char big[100];
  for (size_t i = 0; i < sizeof(big); ++i) {
    big[i] = static_cast<char>('A' + i % 26);
  }
  ASSERT_EQ(::write(a.get(), big, sizeof(big)), 100);
  ASSERT_TRUE(run_until([&] { return stream.size == 105; }));

  EXPECT_EQ(memcmp(stream.data, "hello", 5), 0);
  EXPECT_EQ(memcmp(stream.data + 5, big, sizeof(big)), 0);
  EXPECT_LE(stream.max_chunk, 16u);
  EXPECT_FALSE(stream.finished);
  EXPECT_EQ(stream.more_calls, stream.calls);

  ASSERT_EQ(wio_cancel(reactor_, &stream.op), 0);
  ASSERT_TRUE(run_until([&] { return stream.finished; }));
  EXPECT_EQ(stream.final_result, -ECANCELED);
  EXPECT_EQ(stream.final_flags, 0u);
  EXPECT_EQ(wio_reactor_inflight(reactor_), 0u);
}

TEST_P(ReactorTest, RecvMultishotSeesEndOfStream) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  StreamRequest stream = {};

  ASSERT_EQ(
      wio_recv_multishot(reactor_, &stream.op, b.get(), 16, 4, 0, on_chunk), 0);
  ASSERT_EQ(::write(a.get(), "bye", 3), 3);
  a.reset();
  ASSERT_TRUE(run_until([&] { return stream.finished; }));
  EXPECT_EQ(stream.size, 3u);
  EXPECT_EQ(stream.final_result, 0);
  EXPECT_EQ(wio_reactor_inflight(reactor_), 0u);
}

TEST_P(ReactorTest, RecvMultishotPreservesPartialBuffersAcrossWaits) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  StreamRequest stream = {};
  char expected[4096];
  size_t size = 0;

  ASSERT_EQ(
      wio_recv_multishot(reactor_, &stream.op, b.get(), 64, 2, 0, on_chunk), 0);
  for (size_t round = 0; round < 128; ++round) {
    size_t take = 1 + round * 17 % 47;
    for (size_t i = 0; i < take; ++i) {
      expected[size + i] = static_cast<char>((size + i) % 251);
    }
    ASSERT_EQ(::write(a.get(), expected + size, take),
              static_cast<ssize_t>(take));
    size += take;
    ASSERT_TRUE(run_until([&] { return stream.size == size; }));
    EXPECT_EQ(memcmp(stream.data, expected, size), 0) << "round " << round;
    EXPECT_FALSE(stream.finished);
  }

  a.reset();
  ASSERT_TRUE(run_until([&] { return stream.finished; }));
  EXPECT_EQ(stream.final_result, 0);
  EXPECT_EQ(stream.size, size);
  EXPECT_EQ(wio_reactor_inflight(reactor_), 0u);
}

TEST_P(ReactorTest, RecvMultishotSurvivesRunningOutOfBuffers) {
  Fd a, b;
  ASSERT_TRUE(socket_pair(a, b));
  StreamRequest stream = {};
  char out[4096];
  for (size_t i = 0; i < sizeof(out); ++i) {
    out[i] = static_cast<char>(i % 251);
  }
  ASSERT_EQ(::write(a.get(), out, sizeof(out)), 4096);

  // Two 8-byte buffers: the kernel ends the receive after two chunks, far
  // before the stream is exhausted, and the reactor has to keep it going.
  ASSERT_EQ(
      wio_recv_multishot(reactor_, &stream.op, b.get(), 8, 2, 0, on_chunk), 0);
  ASSERT_TRUE(run_until([&] { return stream.size == sizeof(out); }));
  EXPECT_EQ(memcmp(stream.data, out, sizeof(out)), 0);
  EXPECT_FALSE(stream.finished);

  ASSERT_EQ(wio_cancel(reactor_, &stream.op), 0);
  ASSERT_TRUE(run_until([&] { return stream.finished; }));
  EXPECT_EQ(stream.final_result, -ECANCELED);
}

TEST_P(ReactorTest, ManyMultishotReceivesComeAndGo) {
  // More receives over a reactor's lifetime than there are buffer group ids.
  for (int i = 0; i < 70000; ++i) {
    Fd a, b;
    ASSERT_TRUE(socket_pair(a, b));
    StreamRequest stream = {};
    ASSERT_EQ(
        wio_recv_multishot(reactor_, &stream.op, b.get(), 16, 2, 0, on_chunk),
        0);
    a.reset();
    ASSERT_TRUE(run_until([&] { return stream.finished; })) << "round " << i;
    ASSERT_EQ(stream.final_result, 0);
  }
}

INSTANTIATE_TEST_SUITE_P(Backends, ReactorTest,
                         ::testing::Values(WIO_BACKEND_LIBC, WIO_BACKEND_ALIBC,
                                           WIO_BACKEND_URING),
                         [](const ::testing::TestParamInfo<WioBackend> &info) {
                           return std::string(wio_backend_name(info.param));
                         });
