/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIE_CLIENT_CLIENT_HPP
#define WHEELIE_CLIENT_CLIENT_HPP

#include <wheelie/protocol.h>

#include <wheelio/coro.hpp>
#include <wheelio/reactor.hpp>
#include <wheelio/wheelio.h>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace wheelie {

struct ClientOptions {
  // The server: a name or an IPv4 address, and a port.
  std::string host;
  uint16_t port = 0;
  // The I/O mechanism the client uses on its connection.
  WioBackend backend = WIO_BACKEND_URING;
  // Receive with one multishot receive instead of a receive per chunk.
  bool multishot = false;
  // The most requests in flight at a time.
  unsigned int depth = 64;
  // The connections to the server, which the requests are spread over in
  // turn. A server serves each of them on a thread of its own, so a client
  // with more than one can keep several of them busy at once.
  unsigned int queues = 1;
};

/**
 * A client of a wheelie server: its connections, and the requests in flight on
 * them, each of which a coroutine awaits.
 *
 *     size_t read = co_await client.pread(buffer, size, offset);
 *
 * The client has a reactor of its own, which the application runs:
 * `client.reactor().wait(...)` runs the completions that resume the
 * coroutines. Like the reactor, a client belongs to one thread.
 */
class Client final {
public:
  /**
   * A request to co_await, which gives the number of bytes read or written.
   * It is submitted when it is constructed, and a request that cannot be
   * submitted throws std::system_error from there: -EBUSY if `depth` requests
   * are in flight, -EMSGSIZE for a size larger than WHEELIE_MAX_PAYLOAD_SIZE,
   * -EINVAL for a flag that is unknown or does not apply, or the error that
   * broke the connection. One that fails later throws the same from co_await:
   * the error that broke the connection, or ECANCELED if the client was
   * deleted.
   *
   * A request lives in the awaiting coroutine, and so costs no allocation. The
   * buffer has to stay valid until the request has finished, and so has the
   * coroutine: a request that is awaited must not be destroyed before it
   * completes.
   */
  class Request final {
  public:
    // Use Client::pread() and Client::pwrite().
    Request(Client &client, uint8_t op, unsigned int flags, void *buffer,
            size_t size, uint64_t offset);
    Request(const Request &) = delete;
    Request &operator=(const Request &) = delete;

    bool await_ready() const noexcept { return _done; }

    void await_suspend(std::coroutine_handle<> h) noexcept { _h = h; }

    size_t await_resume();

  private:
    friend class Client;

    std::coroutine_handle<> _h;
    int64_t _result = 0;
    bool _done = false;
  };

  // Connects, and throws std::system_error if it cannot.
  explicit Client(const ClientOptions &options);
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  // Disconnects. The requests still in flight finish with ECANCELED, and the
  // coroutines that await them run, before it returns.
  ~Client();

  wheelio::Reactor &reactor() noexcept { return _reactor; }

  // Reads `size` bytes at `offset` into `buffer`.
  Request pread(void *buffer, size_t size, uint64_t offset) {
    return Request(*this, WHEELIE_OP_READ, 0, buffer, size, offset);
  }

  // Writes `size` bytes at `offset` from `buffer`. `flags` is
  // WHEELIE_FLAG_SYNC to have the data durable when the request finishes, or
  // 0.
  Request pwrite(const void *buffer, size_t size, uint64_t offset,
                 unsigned int flags = 0) {
    return Request(*this, WHEELIE_OP_WRITE, flags, const_cast<void *>(buffer),
                   size, offset);
  }

  // Number of requests submitted and not finished yet.
  unsigned int inflight() const noexcept { return _in_flight; }

private:
  // A request, from its submission until its response. `cid` of the request
  // is the index of its slot, so that a response finds its request at once.
  struct Slot {
    WheelieRequestHeader header;
    // The destination of a read, the source of a write.
    void *buffer;
    Request *request;
    // The connection the request goes out on, and its response comes back on.
    unsigned int connection;
    bool in_use;
  };

  // A connection to the server, with the requests to send on it.
  struct Connection {
    int fd = -1;
    // The requests that have been submitted and not sent, in order.
    std::deque<uint32_t> outgoing;
    std::coroutine_handle<> writer_waiting;
    wheelio::Task<void> session;
  };

  void submit(Request *request, uint8_t op, unsigned int flags, void *buffer,
              size_t size, uint64_t offset);
  void complete(uint32_t index, int64_t res);
  void fail_everything();
  void record_error(int error) noexcept;
  void wake_writer(Connection &connection);

  class WorkAwaiter;

  wheelio::Task<void> session(unsigned int index);
  template <typename Chunks>
  wheelio::Task<void> read_responses(unsigned int index, Chunks &chunks);
  wheelio::Task<void> write_requests(unsigned int index);

  wheelio::Reactor _reactor;
  const bool _multishot;
  std::vector<std::unique_ptr<Connection>> _connections;
  // The connection the next request goes out on.
  unsigned int _next_connection = 0;

  std::vector<Slot> _slots;
  std::vector<uint32_t> _free;
  unsigned int _in_flight = 0;

  // Ends the writers when set. The error that broke the connection, as a
  // negative errno, is the first one recorded; 0 while it is whole.
  bool _finishing = false;
  int _error = 0;
};

} // namespace wheelie

#endif // WHEELIE_CLIENT_CLIENT_HPP
