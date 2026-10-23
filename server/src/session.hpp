#ifndef WHEELIE_SERVER_SESSION_HPP
#define WHEELIE_SERVER_SESSION_HPP

#include <wheelio/coro.hpp>
#include <wheelio/reactor.hpp>

#include <atomic>
#include <unordered_set>
#include <vector>

namespace wheelie {

// What one reactor thread serves with.
struct WorkerContext {
  wheelio::Reactor &reactor;
  int file_fd;
  // The listening socket, on the one reactor that accepts; -1 on the others.
  int listen_fd;
  bool multishot;
  const std::atomic<bool> &stopping;
  // On a reactor that does not accept: the end of a socket pair on which the
  // acceptor sends the descriptors of its connections. -1 on the acceptor.
  int handoff_in;
  // On the acceptor: for each reactor, the other end of its socket pair; the
  // acceptor's own entry is -1.
  std::vector<int> handoff_out;
  // The connections of this reactor, so that stopping can cancel them.
  std::unordered_set<int> connections;
};

// Accepts connections until the reactor is stopped, and gives them to the
// reactors in turn, its own included, so that the connections of a client
// are spread over the threads: the kernel gives them to whichever thread
// waits on a shared listening socket, which leaves some threads without any.
wheelio::Task<void> accept_loop(WorkerContext &context);

// Serves the connections that the acceptor gives to this reactor, each on a
// task of its own, until the reactor is stopped.
wheelio::Task<void> receive_loop(WorkerContext &context);

} // namespace wheelie

#endif // WHEELIE_SERVER_SESSION_HPP
