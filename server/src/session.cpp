#include "session.hpp"

#include <server/aligned_buffer.hpp>
#include <server/request_parser.hpp>

#include <wheelie/chunks.hpp>
#include <wheelie/protocol.h>

#include <wheelio/sync.hpp>

#include <cerrno>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace wheelie {

namespace {

// A response waiting to be sent: its header and, for a read, its data.
struct Pending {
  WheelieResponseHeader header;
  AlignedBuffer data;
  size_t size;
};

// The most responses one send carries. Each takes up to two iovecs, and a send
// takes at most IOV_MAX of them.
constexpr size_t max_batch = 128;

// What a connection's handlers and its writer share.
struct Connection {
  Connection(wheelio::Reactor &reactor, int fd, int file_fd)
      : reactor(reactor), fd(fd), file_fd(file_fd) {}

  wheelio::Reactor &reactor;
  int fd;
  int file_fd;
  // The requests that have been started and not answered yet.
  wheelio::WaitGroup handlers;

  // Responses wait here for the writer, which is the only one to send on the
  // connection: the responses of all the requests that overlap on it are
  // multiplexed onto its one stream, and the writer may send several in one
  // go.
  std::deque<Pending> responses;
  std::coroutine_handle<> writer_waiting;
  bool finishing = false;
  bool broken = false;

  // Hands a response over to the writer, which is woken if it is idle. The
  // data of a read goes along, `size` bytes of it.
  void respond(uint64_t cid, int64_t res, AlignedBuffer data = {},
               size_t size = 0) {
    if (broken) {
      return;
    }
    responses.push_back(Pending{{cid, res}, std::move(data), size});
    wake_writer();
  }

  void wake_writer() {
    if (writer_waiting) {
      std::exchange(writer_waiting, {}).resume();
    }
  }

  // Suspends the writer until there is something to send, or there will be
  // nothing more.
  class WorkAwaiter final {
  private:
    Connection &_connection;

  public:
    explicit WorkAwaiter(Connection &connection) : _connection(connection) {}

    bool await_ready() const noexcept {
      return !_connection.responses.empty() || _connection.finishing;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
      _connection.writer_waiting = h;
    }

    void await_resume() const noexcept {}
  };

  WorkAwaiter work() { return WorkAwaiter(*this); }
};

// Sends the responses as they are handed over until the connection is
// finishing and none is left. What a failed send leaves behind is dropped, and
// the connection is ended: its requests cannot be answered any more.
wheelio::Task<void> write_responses(Connection &connection) {
  std::vector<Pending> batch;
  std::vector<iovec> iov;

  for (;;) {
    co_await connection.work();
    if (connection.responses.empty()) {
      co_return;
    }

    batch.clear();
    iov.clear();
    while (!connection.responses.empty() && batch.size() < max_batch) {
      batch.push_back(std::move(connection.responses.front()));
      connection.responses.pop_front();
    }
    for (Pending &pending : batch) {
      iov.push_back({&pending.header, sizeof(pending.header)});
      if (pending.size > 0) {
        iov.push_back({pending.data.data(), pending.size});
      }
    }

    try {
      size_t index = 0;
      while (index < iov.size()) {
        msghdr message = {};
        message.msg_iov = &iov[index];
        message.msg_iovlen = iov.size() - index;
        size_t sent =
            co_await connection.reactor.sendmsg(connection.fd, &message);

        // A part of what was left went out; what is left now starts after it.
        while (sent > 0 && index < iov.size()) {
          if (sent >= iov[index].iov_len) {
            sent -= iov[index].iov_len;
            ++index;
          } else {
            iov[index].iov_base =
                static_cast<std::byte *>(iov[index].iov_base) + sent;
            iov[index].iov_len -= sent;
            sent = 0;
          }
        }
      }
    } catch (...) { // NOLINT: the connection is going away
      connection.broken = true;
      connection.responses.clear();
      // Nothing more can be answered: end the connection's receive too.
      connection.reactor.cancel_fd(connection.fd);
      co_return;
    }
  }
}

// The three handlers below are the whole life of one request: they do what it
// asks, hand the response to the writer and are done. Each tells the
// connection that it has finished as the very last thing it does: that may let
// the connection go away.

wheelio::DetachedTask reject_read(Connection &connection, uint64_t cid) {
  connection.respond(cid, -EMSGSIZE);
  connection.handlers.done();
  co_return;
}

wheelio::DetachedTask handle_read(Connection &connection, uint64_t cid,
                                  uint64_t offset, uint64_t size) {
  AlignedBuffer buffer;
  int64_t res;
  try {
    buffer = AlignedBuffer(size);
    res = static_cast<int64_t>(co_await connection.reactor.pread(
        connection.file_fd, buffer.data(), buffer.size(), offset));
  } catch (const std::bad_alloc &) {
    res = -ENOMEM;
  } catch (const std::system_error &e) {
    res = -e.code().value();
  }
  connection.respond(cid, res, std::move(buffer),
                     res > 0 ? static_cast<size_t>(res) : 0);
  connection.handlers.done();
}

wheelio::DetachedTask handle_write(Connection &connection, uint64_t cid,
                                   uint64_t offset, AlignedBuffer payload,
                                   bool durable) {
  int64_t res;
  try {
    res = static_cast<int64_t>(co_await connection.reactor.pwrite(
        connection.file_fd, payload.data(), payload.size(), offset,
        durable ? WIO_WRITE_DSYNC : 0));
  } catch (const std::system_error &e) {
    res = -e.code().value();
  }
  connection.respond(cid, res);
  connection.handlers.done();
}

void dispatch(Connection &connection, Request &&request) {
  const WheelieRequestHeader &header = request.header;
  connection.handlers.add();
  if (header.op == WHEELIE_OP_READ) {
    if (header.size > WHEELIE_MAX_PAYLOAD_SIZE) {
      reject_read(connection, header.cid);
    } else {
      handle_read(connection, header.cid, header.offset, header.size);
    }
  } else {
    handle_write(connection, header.cid, header.offset,
                 std::move(request.payload),
                 (header.flags & WHEELIE_FLAG_SYNC) != 0);
  }
}

template <typename Chunks>
wheelio::Task<void> read_requests(Connection &connection, Chunks &chunks) {
  RequestParser parser;
  while (auto chunk = co_await chunks.next()) {
    parser.feed(*chunk, [&](Request &&request) {
      dispatch(connection, std::move(request));
    });
  }
  if (parser.mid_request()) {
    throw ProtocolError(
        "wheelie: connection closed in the middle of a request");
  }
}

wheelio::Task<void> serve_connection(WorkerContext &context, int fd) {
  Connection connection(context.reactor, fd, context.file_fd);
  context.connections.insert(fd);
  wheelio::Task<void> writer = write_responses(connection);

  try {
    if (context.multishot) {
      MultishotChunks chunks(context.reactor, fd);
      co_await read_requests(connection, chunks);
    } else {
      SingleShotChunks chunks(context.reactor, fd);
      co_await read_requests(connection, chunks);
    }
  } catch (...) { // NOLINT: ended or broken, either way nothing more to read
  }

  // Every request answers into the queue; once they have, the writer sends
  // what is left and is done.
  co_await connection.handlers.wait();
  connection.finishing = true;
  connection.wake_writer();
  co_await writer;

  context.connections.erase(fd);
  try {
    co_await context.reactor.close(fd);
  } catch (...) { // NOLINT: the descriptor is unusable either way
  }
}

} // namespace

wheelio::Task<void> accept_loop(WorkerContext &context) {
  size_t next = 0;
  while (!context.stopping.load(std::memory_order_relaxed)) {
    int fd;
    try {
      fd = co_await context.reactor.accept(context.listen_fd);
    } catch (const std::system_error &e) {
      int err = e.code().value();
      if (err == ECANCELED || err == EBADF || err == EINVAL ||
          context.stopping.load(std::memory_order_relaxed)) {
        co_return;
      }
      // A connection that was reset before it could be accepted, and the
      // like: nothing to do but accept the next one.
      continue;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // A connection accepted while the reactor is draining would be missed by
    // the cancellation of the ones it has.
    if (context.stopping.load(std::memory_order_relaxed)) {
      ::close(fd);
      co_return;
    }

    size_t target = next++;
    if (!context.handoff_out.empty()) {
      target %= context.handoff_out.size();
    }
    if (target == 0 || context.handoff_out.empty()) {
      wheelio::spawn(serve_connection(context, fd));
    } else if (::send(context.handoff_out[target], &fd, sizeof(fd),
                      MSG_NOSIGNAL | MSG_DONTWAIT) !=
               static_cast<ssize_t>(sizeof(fd))) {
      ::close(fd);
    }
  }
}

wheelio::Task<void> receive_loop(WorkerContext &context) {
  // A stream does not keep the descriptors apart: what is left of one at the
  // end of a read is the start of the next.
  char buffer[16 * sizeof(int)];
  size_t have = 0;
  while (!context.stopping.load(std::memory_order_relaxed)) {
    size_t got;
    try {
      got = co_await context.reactor.recv(context.handoff_in, buffer + have,
                                          sizeof(buffer) - have);
    } catch (const std::system_error &) {
      co_return;
    }
    if (got == 0) {
      co_return;
    }
    have += got;

    size_t used = 0;
    for (; used + sizeof(int) <= have; used += sizeof(int)) {
      int fd;
      std::memcpy(&fd, buffer + used, sizeof(fd));
      // Likewise for one that arrives while the reactor is draining.
      if (context.stopping.load(std::memory_order_relaxed)) {
        ::close(fd);
      } else {
        wheelio::spawn(serve_connection(context, fd));
      }
    }
    std::memmove(buffer, buffer + used, have - used);
    have -= used;
  }
}

} // namespace wheelie
