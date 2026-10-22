#include <client/client.hpp>
#include <client/response_parser.hpp>

#include <wheelie/chunks.hpp>
#include <wheelie/protocol.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace wheelie {

namespace {

// The operations a client has in flight at once: the receive of the responses
// and the send of the requests, and the connect before them.
constexpr unsigned int reactor_depth = 8;

// The most requests one send carries. Each takes up to two iovecs, and a send
// takes at most IOV_MAX of them.
constexpr size_t max_batch = 128;

constexpr int connect_checks = 100;
constexpr int connect_check_ms = 100;

// How long a client that is deleted gives its connection to wind down.
constexpr int drain_checks = 50;
constexpr int drain_check_ms = 100;

[[noreturn]] void throw_errno(int err, const char *what) {
  throw std::system_error(err, std::generic_category(), what);
}

int error_of(const std::exception_ptr &exception) noexcept {
  try {
    std::rethrow_exception(exception);
  } catch (const std::system_error &e) {
    return -e.code().value();
  } catch (const ProtocolError &) {
    return -EPROTO;
  } catch (...) {
    return -EIO;
  }
}

// An IPv4 address for `host`, which is a name or an address.
sockaddr_in resolve(const char *host, uint16_t port) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *found = nullptr;
  int ret = getaddrinfo(host, std::to_string(port).c_str(), &hints, &found);
  if (ret != 0 || found == nullptr) {
    throw std::system_error(EHOSTUNREACH, std::generic_category(),
                            std::string("cannot resolve '") + host +
                                "': " + gai_strerror(ret));
  }
  sockaddr_in address = {};
  std::memcpy(&address, found->ai_addr, sizeof(address));
  freeaddrinfo(found);
  return address;
}

} // namespace

// Suspends the writer until there is something to send, or it is to finish.
class Client::WorkAwaiter final {
private:
  Client &_client;

public:
  explicit WorkAwaiter(Client &client) : _client(client) {}

  bool await_ready() const noexcept {
    return !_client._outgoing.empty() || _client._finishing;
  }

  void await_suspend(std::coroutine_handle<> h) noexcept {
    _client._writer_waiting = h;
  }

  void await_resume() const noexcept {}
};

Client::Client(const ClientOptions &options)
    : _reactor(options.backend, reactor_depth), _multishot(options.multishot),
      _slots(options.depth), _session() {
  if (options.depth == 0) {
    throw std::invalid_argument("wheelie: depth must not be 0");
  }
  _free.reserve(options.depth);
  for (uint32_t i = options.depth; i > 0; --i) {
    _free.push_back(i - 1);
  }

  sockaddr_in address = resolve(options.host.c_str(), options.port);
  _fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (_fd < 0) {
    throw_errno(errno, "socket");
  }

  try {
    // The address is a parameter of the coroutine, so it lives as long as the
    // connect does.
    wheelio::Task<void> connecting =
        [](Client &client, sockaddr_in address) -> wheelio::Task<void> {
      co_await client._reactor.connect(
          client._fd, reinterpret_cast<const sockaddr *>(&address),
          sizeof(address));
    }(*this, address);

    for (int i = 0; i < connect_checks && !connecting.done(); ++i) {
      _reactor.wait(connect_check_ms);
    }
    if (!connecting.done()) {
      _reactor.cancel_fd(_fd);
      for (int i = 0; i < drain_checks && !connecting.done(); ++i) {
        _reactor.wait(drain_check_ms);
      }
      throw_errno(ETIMEDOUT, "connect");
    }
    connecting.get();

    int one = 1;
    setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  } catch (...) {
    ::close(_fd);
    _fd = -1;
    throw;
  }

  _session = session();
}

Client::~Client() {
  _finishing = true;
  record_error(-ECANCELED);
  if (_fd >= 0) {
    _reactor.cancel_fd(_fd);
  }
  wake_writer();
  try {
    for (int i = 0; i < drain_checks && !_session.done(); ++i) {
      _reactor.wait(drain_check_ms);
    }
  } catch (...) { // NOLINT: nothing to do about it while closing
  }

  if (!_session.done()) {
    // Something is still suspended on an operation of the reactor: destroying
    // the session would resume a frame that is gone, so it is left as it is.
    new wheelio::Task<void>(std::move(_session));
  }
  if (_fd >= 0) {
    ::close(_fd);
  }
}

void Client::record_error(int error) noexcept {
  if (_error == 0) {
    _error = error;
  }
}

void Client::wake_writer() {
  if (_writer_waiting) {
    std::exchange(_writer_waiting, {}).resume();
  }
}

Client::Request::Request(Client &client, uint8_t op, unsigned int flags,
                         void *buffer, size_t size, uint64_t offset) {
  client.submit(this, op, flags, buffer, size, offset);
}

size_t Client::Request::await_resume() {
  if (_result < 0) {
    throw_errno(static_cast<int>(-_result), "wheelie request");
  }
  return static_cast<size_t>(_result);
}

void Client::submit(Request *request, uint8_t op, unsigned int flags,
                    void *buffer, size_t size, uint64_t offset) {
  if (_error != 0) {
    throw_errno(-_error, "wheelie connection");
  }
  if (size > WHEELIE_MAX_PAYLOAD_SIZE) {
    throw_errno(EMSGSIZE, "wheelie request");
  }
  unsigned int allowed = op == WHEELIE_OP_WRITE ? WHEELIE_FLAG_SYNC : 0;
  if (flags & ~allowed) {
    throw_errno(EINVAL, "wheelie request");
  }
  if (_free.empty()) {
    throw_errno(EBUSY, "wheelie request");
  }

  uint32_t index = _free.back();
  _free.pop_back();
  Slot &slot = _slots[index];
  slot.header = {index, offset, size, op, static_cast<uint8_t>(flags)};
  slot.buffer = buffer;
  slot.request = request;
  slot.in_use = true;
  ++_in_flight;
  _outgoing.push_back(index);
  wake_writer();
}

void Client::complete(uint32_t index, int64_t res) {
  Slot &slot = _slots[index];
  Request *request = slot.request;
  slot.in_use = false;
  _free.push_back(index);
  --_in_flight;
  request->_result = res;
  request->_done = true;
  // The slot is free before the coroutine runs, which may submit a request.
  // Nothing of the request is touched after that: the coroutine may release
  // it.
  if (request->_h) {
    std::exchange(request->_h, {}).resume();
  }
}

// Finishes the requests that can no longer be answered, with the error that
// broke the connection.
void Client::fail_everything() {
  _outgoing.clear();
  for (uint32_t index = 0; index < _slots.size(); ++index) {
    if (_slots[index].in_use) {
      complete(index, _error != 0 ? _error : -ECONNRESET);
    }
  }
}

wheelio::Task<void> Client::write_requests() {
  std::vector<uint32_t> batch;
  std::vector<iovec> iov;

  for (;;) {
    co_await WorkAwaiter(*this);
    if (_finishing || _outgoing.empty()) {
      co_return;
    }

    batch.clear();
    iov.clear();
    while (!_outgoing.empty() && batch.size() < max_batch) {
      batch.push_back(_outgoing.front());
      _outgoing.pop_front();
    }
    for (uint32_t index : batch) {
      Slot &slot = _slots[index];
      iov.push_back({&slot.header, sizeof(slot.header)});
      if (slot.header.op == WHEELIE_OP_WRITE && slot.header.size > 0) {
        iov.push_back({slot.buffer, static_cast<size_t>(slot.header.size)});
      }
    }

    try {
      size_t at = 0;
      while (at < iov.size()) {
        msghdr message = {};
        message.msg_iov = &iov[at];
        message.msg_iovlen = iov.size() - at;
        size_t sent = co_await _reactor.sendmsg(_fd, &message);

        // A part of what was left went out; what is left now starts after it.
        while (sent > 0 && at < iov.size()) {
          if (sent >= iov[at].iov_len) {
            sent -= iov[at].iov_len;
            ++at;
          } else {
            iov[at].iov_base =
                static_cast<std::byte *>(iov[at].iov_base) + sent;
            iov[at].iov_len -= sent;
            sent = 0;
          }
        }
      }
    } catch (...) {
      record_error(error_of(std::current_exception()));
      _reactor.cancel_fd(_fd);
      co_return;
    }
  }
}

template <typename Chunks>
wheelio::Task<void> Client::read_responses(Chunks &chunks) {
  ResponseParser parser;
  auto lookup = [this](uint64_t cid) -> std::optional<ResponseTarget> {
    if (cid >= _slots.size() || !_slots[cid].in_use) {
      return std::nullopt;
    }
    Slot &slot = _slots[cid];
    if (slot.header.op == WHEELIE_OP_READ) {
      return ResponseTarget{static_cast<std::byte *>(slot.buffer),
                            static_cast<size_t>(slot.header.size)};
    }
    return ResponseTarget{nullptr, 0};
  };
  auto on_response = [this](uint64_t cid, int64_t res) {
    complete(static_cast<uint32_t>(cid), res);
  };

  while (auto chunk = co_await chunks.next()) {
    parser.feed(*chunk, lookup, on_response);
  }
  if (parser.mid_response()) {
    throw ProtocolError(
        "wheelie: connection closed in the middle of a response");
  }
  // The server closed the connection.
  record_error(-ECONNRESET);
}

wheelio::Task<void> Client::session() {
  wheelio::Task<void> writer = write_requests();

  try {
    if (_multishot) {
      MultishotChunks chunks(_reactor, _fd);
      co_await read_responses(chunks);
    } else {
      SingleShotChunks chunks(_reactor, _fd);
      co_await read_responses(chunks);
    }
  } catch (...) { // NOLINT: whatever it was is recorded
    record_error(error_of(std::current_exception()));
  }

  // Nothing is received any more, so nothing is worth sending: the writer is
  // told to finish, and a send that is stuck is cancelled.
  _finishing = true;
  _reactor.cancel_fd(_fd);
  wake_writer();
  co_await writer;

  fail_everything();
}

} // namespace wheelie
