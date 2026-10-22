/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIO_CORO_HPP
#define WHEELIO_CORO_HPP

#include <coroutine>
#include <exception>
#include <utility>
#include <variant>

namespace wheelio {

template <typename T = void> class Task;

namespace detail {

// What the promise types of Task<T> and Task<void> share: the coroutine
// starts at once, and finishing it hands control back to whoever awaits it by
// symmetric transfer, so that chains of awaited tasks do not grow the stack.
class TaskPromiseBase {
private:
  class FinalAwaiter {
  public:
    bool await_ready() noexcept { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
      return static_cast<TaskPromiseBase &>(h.promise())._continuation;
    }

    void await_resume() noexcept {}
  };

  std::coroutine_handle<> _continuation = std::noop_coroutine();

public:
  std::suspend_never initial_suspend() noexcept { return {}; }

  FinalAwaiter final_suspend() noexcept { return {}; }

  void set_continuation(std::coroutine_handle<> h) noexcept {
    _continuation = h;
  }
};

// Exceptions that left a detached coroutine. They cannot be thrown from where
// they happen: that is inside a reactor callback, a C frame. Reactor::wait()
// throws them once it is back in C++.
inline thread_local std::exception_ptr detached_exception;

} // namespace detail

/**
 * A coroutine with a result. It runs on the caller's stack up to its first
 * real suspension, so work that does not wait for anything finishes inside
 * the call that started it.
 *
 * A task must not be destroyed while it is suspended on an operation that is
 * still in flight: the operation will resume a frame that is no longer
 * there. Await it, or cancel what it waits on and let that finish first.
 */
template <typename T> class [[nodiscard]] Task final {
public:
  class promise_type : public detail::TaskPromiseBase {
  public:
    Task get_return_object() noexcept {
      return Task(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    template <typename U> void return_value(U &&value) {
      _result.template emplace<T>(std::forward<U>(value));
    }

    void unhandled_exception() noexcept {
      _result.template emplace<std::exception_ptr>(std::current_exception());
    }

    std::variant<std::monostate, T, std::exception_ptr> _result;
  };

private:
  using handle_type = std::coroutine_handle<promise_type>;

  explicit Task(handle_type h) noexcept : _h(h) {}

  handle_type _h;

public:
  Task() noexcept = default;
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  Task(Task &&other) noexcept : _h(std::exchange(other._h, {})) {}

  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (_h) {
        _h.destroy();
      }
      _h = std::exchange(other._h, {});
    }
    return *this;
  }

  ~Task() {
    if (_h) {
      _h.destroy();
    }
  }

  bool done() const noexcept { return !_h || _h.done(); }

  bool await_ready() const noexcept { return done(); }

  void await_suspend(std::coroutine_handle<> awaiting) noexcept {
    _h.promise().set_continuation(awaiting);
  }

  T await_resume() { return get(); }

  // The result of a finished task; throws what the task threw.
  T get() {
    auto &result = _h.promise()._result;
    if (std::holds_alternative<std::exception_ptr>(result)) {
      std::rethrow_exception(std::get<std::exception_ptr>(result));
    }
    return std::move(std::get<T>(result));
  }
};

template <> class [[nodiscard]] Task<void> final {
public:
  class promise_type : public detail::TaskPromiseBase {
  public:
    Task get_return_object() noexcept {
      return Task(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    void return_void() noexcept {}

    void unhandled_exception() noexcept {
      _exception = std::current_exception();
    }

    std::exception_ptr _exception;
  };

private:
  using handle_type = std::coroutine_handle<promise_type>;

  explicit Task(handle_type h) noexcept : _h(h) {}

  handle_type _h;

public:
  Task() noexcept = default;
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  Task(Task &&other) noexcept : _h(std::exchange(other._h, {})) {}

  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (_h) {
        _h.destroy();
      }
      _h = std::exchange(other._h, {});
    }
    return *this;
  }

  ~Task() {
    if (_h) {
      _h.destroy();
    }
  }

  bool done() const noexcept { return !_h || _h.done(); }

  bool await_ready() const noexcept { return done(); }

  void await_suspend(std::coroutine_handle<> awaiting) noexcept {
    _h.promise().set_continuation(awaiting);
  }

  void await_resume() { get(); }

  void get() {
    if (_h.promise()._exception) {
      std::rethrow_exception(_h.promise()._exception);
    }
  }
};

/**
 * The return type of a coroutine nobody waits for. Its frame destroys itself
 * when the body ends.
 */
class DetachedTask final {
public:
  class promise_type {
  public:
    DetachedTask get_return_object() noexcept { return {}; }

    std::suspend_never initial_suspend() noexcept { return {}; }

    std::suspend_never final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() noexcept {
      detail::detached_exception = std::current_exception();
    }
  };

  // Throws the exception that left a detached coroutine, if one did. Call it
  // from C++ code right after running or resuming coroutines, as
  // Reactor::wait() and spawn() do.
  static void rethrow_if_pending() {
    if (detail::detached_exception) {
      std::rethrow_exception(std::exchange(detail::detached_exception, {}));
    }
  }
};

namespace detail {

template <typename T> DetachedTask run_detached(Task<T> task) {
  co_await std::move(task);
}

} // namespace detail

/**
 * Starts `task` and lets it run on its own. What it throws is not lost: it is
 * thrown by the next Reactor::wait().
 */
template <typename T> void spawn(Task<T> task) {
  detail::run_detached(std::move(task));
  DetachedTask::rethrow_if_pending();
}

} // namespace wheelio

#endif // WHEELIO_CORO_HPP
