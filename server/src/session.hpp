#ifndef WHEELIE_SERVER_SESSION_HPP
#define WHEELIE_SERVER_SESSION_HPP

#include <wheelio/coro.hpp>
#include <wheelio/reactor.hpp>

#include <atomic>
#include <unordered_set>

namespace wheelie {

// What one reactor thread serves with.
struct WorkerContext {
  wheelio::Reactor &reactor;
  int file_fd;
  int listen_fd;
  bool multishot;
  const std::atomic<bool> &stopping;
  // The connections of this reactor, so that stopping can cancel them.
  std::unordered_set<int> connections;
};

// Accepts connections on the shared listening socket until the reactor is
// stopped, serving each one on a task of its own.
wheelio::Task<void> accept_loop(WorkerContext &context);

} // namespace wheelie

#endif // WHEELIE_SERVER_SESSION_HPP
