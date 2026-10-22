/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIO_SYNC_HPP
#define WHEELIO_SYNC_HPP

#include <coroutine>
#include <utility>

namespace wheelio {

/**
 * Lets one coroutine wait for a number of others to finish: add() before each
 * is started, done() as each finishes, and `co_await wait()` returns once the
 * count is back to zero. At most one coroutine waits at a time.
 */
class WaitGroup final {
public:
  void add(int count = 1) noexcept { _count += count; }

  void done() noexcept {
    if (--_count <= 0 && _waiter) {
      std::exchange(_waiter, {}).resume();
    }
  }

  class Awaiter final {
  private:
    WaitGroup *_group;

  public:
    explicit Awaiter(WaitGroup *group) noexcept : _group(group) {}

    bool await_ready() const noexcept { return _group->_count <= 0; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
      _group->_waiter = h;
    }

    void await_resume() const noexcept {}
  };

  Awaiter wait() noexcept { return Awaiter(this); }

private:
  int _count = 0;
  std::coroutine_handle<> _waiter;
};

} // namespace wheelio

#endif // WHEELIO_SYNC_HPP
