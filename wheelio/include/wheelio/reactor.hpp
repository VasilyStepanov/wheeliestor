/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIO_REACTOR_HPP
#define WHEELIO_REACTOR_HPP

#include <wheelio/coro.hpp>
#include <wheelio/wheelio.h>

#include <cstddef>
#include <optional>
#include <span>
#include <system_error>
#include <type_traits>
#include <vector>

namespace wheelio {

namespace detail {

[[noreturn]] inline void throw_errno(int err) {
  throw std::system_error(err, std::generic_category());
}

} // namespace detail

/**
 * A single-shot operation to co_await. It is submitted when it is
 * constructed, and its WioOp lives in the awaitable itself, on the frame of
 * the coroutine that awaits it, so it costs no allocation. Nothing completes
 * before the next Reactor::wait(), but an operation may well complete before
 * it is awaited: submit two, await the second one first.
 *
 * Every error, a failed submission and a failed operation alike, is thrown as
 * std::system_error: from the call that submits, or from co_await.
 *
 * The operation, its buffers and its arguments must stay alive until it has
 * completed.
 */
template <typename T> class Operation final {
private:
  WioOp _op;
  WioReactor *_reactor;
  std::coroutine_handle<> _h;
  int _result;
  bool _done;

  static void complete(WioOp *op, int result, unsigned int) noexcept {
    Operation *self = wio_container_of(op, Operation, _op);
    self->_result = result;
    self->_done = true;
    if (self->_h) {
      // The coroutine may release this object as it runs: nothing is touched
      // after the resume.
      std::exchange(self->_h, {}).resume();
    }
  }

public:
  // `submit` calls the wio_* function: it is given the WioOp and the callback.
  template <typename Submit>
  Operation(WioReactor *reactor, Submit &&submit)
      : _op(), _reactor(reactor), _h(), _result(0), _done(false) {
    int ret = submit(&_op, &Operation::complete);
    if (ret < 0) {
      detail::throw_errno(-ret);
    }
  }

  Operation(const Operation &) = delete;
  Operation &operator=(const Operation &) = delete;

  bool await_ready() const noexcept { return _done; }

  void await_suspend(std::coroutine_handle<> h) noexcept { _h = h; }

  T await_resume() {
    if (_result < 0) {
      detail::throw_errno(-_result);
    }
    return static_cast<T>(_result);
  }

  // Asks for the operation to be cancelled; awaiting it then throws ECANCELED
  // unless it completed on its own first. False if it has completed already.
  bool cancel() {
    if (_done) {
      return false;
    }
    return wio_cancel(_reactor, &_op) == 0;
  }
};

/**
 * A stream of the chunks received from a socket by one multishot receive, for
 * `while (auto chunk = co_await stream.next())`.
 *
 * A chunk is a view of the reactor's receive buffer, which is taken back as
 * soon as the coroutine suspends again: use the chunk, or copy it, before
 * awaiting anything else. When the coroutine is not waiting in next(), the
 * chunks that arrive are copied and kept for the next call. The stream does
 * not slow the sender down, so keep calling next().
 *
 * next() ends with std::nullopt at the end of the stream and throws
 * std::system_error on an error, ECANCELED after cancel() included.
 *
 * Destroying a stream that has not ended cancels it; what is left of it frees
 * itself when the reactor reports that the receive is over.
 */
class RecvStream final {
private:
  struct State;

  // Standard layout, so that wio_container_of() finds it from the WioOp.
  struct Core {
    WioOp op;
    State *state;
  };

  struct State {
    Core core;
    WioReactor *reactor;
    std::coroutine_handle<> waiter;
    const std::byte *direct_data;
    size_t direct_size;
    std::vector<std::byte> backlog;
    std::vector<std::byte> serving;
    bool finished;
    int error;
    bool detached;
  };

  State *_state;

  static void on_chunk(WioOp *op, const void *data, int result,
                       unsigned int flags) noexcept {
    Core *core = wio_container_of(op, Core, op);
    State *state = core->state;
    bool final = !(flags & WIO_F_MORE);

    if (state->detached) {
      if (final) {
        delete state;
      }
      return;
    }

    if (result > 0) {
      const std::byte *bytes = static_cast<const std::byte *>(data);
      if (state->waiter) {
        state->direct_data = bytes;
        state->direct_size = static_cast<size_t>(result);
        std::exchange(state->waiter, {}).resume();
        // The coroutine may have destroyed the stream, which only detaches
        // it while the receive is still going.
        if (!state->detached) {
          state->direct_data = nullptr;
        }
      } else {
        state->backlog.insert(state->backlog.end(), bytes, bytes + result);
      }
    }

    if (final) {
      state->finished = true;
      state->error = result < 0 ? -result : 0;
      if (state->waiter) {
        std::exchange(state->waiter, {}).resume();
      }
    }
  }

public:
  RecvStream(WioReactor *reactor, int fd, size_t entry_size,
             unsigned int entries, unsigned int flags)
      : _state(new State{}) {
    _state->core.state = _state;
    _state->reactor = reactor;
    int ret = wio_recv_multishot(reactor, &_state->core.op, fd, entry_size,
                                 entries, flags, &RecvStream::on_chunk);
    if (ret < 0) {
      delete _state;
      detail::throw_errno(-ret);
    }
  }

  RecvStream(const RecvStream &) = delete;
  RecvStream &operator=(const RecvStream &) = delete;

  RecvStream(RecvStream &&other) noexcept
      : _state(std::exchange(other._state, nullptr)) {}

  RecvStream &operator=(RecvStream &&other) noexcept {
    if (this != &other) {
      release();
      _state = std::exchange(other._state, nullptr);
    }
    return *this;
  }

  ~RecvStream() { release(); }

  class Next final {
  private:
    State *_state;

  public:
    explicit Next(State *state) noexcept : _state(state) {}

    bool await_ready() const noexcept {
      return !_state->backlog.empty() || _state->finished;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
      _state->waiter = h;
    }

    std::optional<std::span<const std::byte>> await_resume() {
      State &state = *_state;
      if (state.direct_data != nullptr) {
        return std::span<const std::byte>(state.direct_data, state.direct_size);
      }
      if (!state.backlog.empty()) {
        state.serving.clear();
        state.serving.swap(state.backlog);
        return std::span<const std::byte>(state.serving);
      }
      if (state.error != 0) {
        detail::throw_errno(state.error);
      }
      return std::nullopt;
    }
  };

  Next next() noexcept { return Next(_state); }

  // Asks for the receive to be cancelled; next() then throws ECANCELED. False
  // if the stream has ended already.
  bool cancel() {
    if (_state->finished) {
      return false;
    }
    return wio_cancel(_state->reactor, &_state->core.op) == 0;
  }

private:
  void release() noexcept {
    if (_state == nullptr) {
      return;
    }
    if (_state->finished) {
      delete _state;
    } else {
      _state->detached = true;
      wio_cancel(_state->reactor, &_state->core.op);
    }
    _state = nullptr;
  }
};

/**
 * The coroutine face of a WioReactor. Like the C one, it belongs to one
 * thread, and it must outlive every operation started on it.
 */
class Reactor final {
private:
  WioReactor *_reactor;

public:
  Reactor(WioBackend backend, unsigned int depth) : _reactor(nullptr) {
    int ret = wio_reactor_create(backend, depth, &_reactor);
    if (ret < 0) {
      detail::throw_errno(-ret);
    }
  }

  Reactor(const Reactor &) = delete;
  Reactor &operator=(const Reactor &) = delete;

  ~Reactor() { wio_reactor_delete(_reactor); }

  WioReactor *get() noexcept { return _reactor; }

  unsigned int inflight() const noexcept {
    return wio_reactor_inflight(_reactor);
  }

  // Runs completions, which runs the coroutines waiting for them. Throws what
  // a spawned task threw, or std::system_error if waiting failed. Returns the
  // number of callbacks run.
  int wait(int timeout_ms) {
    int ret = wio_wait(_reactor, timeout_ms);
    DetachedTask::rethrow_if_pending();
    if (ret < 0) {
      detail::throw_errno(-ret);
    }
    return ret;
  }

  // Cancels every operation in flight on `fd`.
  void cancel_fd(int fd) { wio_cancel_fd(_reactor, fd); }

  Operation<int> accept(int fd, sockaddr *addr = nullptr,
                        socklen_t *addrlen = nullptr) {
    return Operation<int>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_accept(_reactor, op, fd, addr, addrlen, cb);
    });
  }

  Operation<int> connect(int fd, const sockaddr *addr, socklen_t addrlen) {
    return Operation<int>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_connect(_reactor, op, fd, addr, addrlen, cb);
    });
  }

  Operation<int> close(int fd) {
    return Operation<int>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_close(_reactor, op, fd, cb);
    });
  }

  Operation<size_t> recv(int fd, void *buf, size_t size,
                         unsigned int flags = 0) {
    return Operation<size_t>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_recv(_reactor, op, fd, buf, size, flags, cb);
    });
  }

  RecvStream recv_multishot(int fd, size_t entry_size, unsigned int entries,
                            unsigned int flags = 0) {
    return RecvStream(_reactor, fd, entry_size, entries, flags);
  }

  Operation<size_t> send(int fd, const void *buf, size_t size,
                         unsigned int flags = 0) {
    return Operation<size_t>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_send(_reactor, op, fd, buf, size, flags, cb);
    });
  }

  Operation<size_t> sendmsg(int fd, const msghdr *msg, unsigned int flags = 0) {
    return Operation<size_t>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_sendmsg(_reactor, op, fd, msg, flags, cb);
    });
  }

  Operation<size_t> pread(int fd, void *buf, size_t size, uint64_t offset) {
    return Operation<size_t>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_pread(_reactor, op, fd, buf, size, offset, cb);
    });
  }

  Operation<size_t> pwrite(int fd, const void *buf, size_t size,
                           uint64_t offset, unsigned int flags = 0) {
    return Operation<size_t>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_pwrite(_reactor, op, fd, buf, size, offset, flags, cb);
    });
  }

  Operation<int> fsync(int fd, bool datasync = false) {
    return Operation<int>(_reactor, [&](WioOp *op, WioCallback cb) {
      return wio_fsync(_reactor, op, fd, datasync, cb);
    });
  }
};

} // namespace wheelio

#endif // WHEELIO_REACTOR_HPP
