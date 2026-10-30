#include <server/server.hpp>

#include "session.hpp"

#include <wheelio/reactor.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <exception>
#include <future>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace wheelie {

namespace {

// How long a reactor waits at most before it looks at whether it has to stop.
constexpr int stop_check_ms = 100;

// How long a stopping reactor gives its connections to finish.
constexpr int drain_checks = 50;

[[noreturn]] void throw_errno(int err, const char *what) {
  throw std::system_error(err, std::generic_category(), what);
}

int open_file(const std::string &path, bool direct) {
  int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | (direct ? O_DIRECT : 0));
  if (fd < 0) {
    throw_errno(errno, ("open " + path).c_str());
  }
  return fd;
}

int open_listener(const std::string &host, uint16_t port) {
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    throw std::invalid_argument("wheelie: invalid bind address '" + host + "'");
  }

  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    throw_errno(errno, "socket");
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
      ::listen(fd, 256) < 0) {
    int err = errno;
    ::close(fd);
    throw_errno(err, "bind/listen");
  }
  return fd;
}

uint16_t port_of(int fd) {
  sockaddr_in address = {};
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) < 0) {
    throw_errno(errno, "getsockname");
  }
  return ntohs(address.sin_port);
}

} // namespace

struct Server::Impl {
  ServerConfig config;
  int file_fd = -1;
  int listen_fd = -1;
  std::atomic<bool> stopping{false};
  std::vector<std::thread> workers;
  // For each reactor but the acceptor, a socket pair that the acceptor gives
  // the descriptors of its connections on: it sends on `handoff_out` and the
  // reactor receives on `handoff_in`. The entry of the acceptor is -1.
  std::vector<int> handoff_in;
  std::vector<int> handoff_out;
  std::mutex error_mutex;
  std::exception_ptr error;

  void fail(std::exception_ptr e) {
    {
      std::lock_guard<std::mutex> lock(error_mutex);
      if (!error) {
        error = e;
      }
    }
    stopping.store(true);
  }

  // Cancels what the reactor has in flight and gives it a moment to finish.
  void drain(WorkerContext &context) {
    if (context.listen_fd >= 0) {
      context.reactor.cancel_fd(context.listen_fd);
    }
    if (context.handoff_in >= 0) {
      context.reactor.cancel_fd(context.handoff_in);
    }
    std::vector<int> connections(context.connections.begin(),
                                 context.connections.end());
    for (int fd : connections) {
      context.reactor.cancel_fd(fd);
    }
    for (int i = 0; i < drain_checks && context.reactor.inflight() > 0; ++i) {
      context.reactor.wait(stop_check_ms);
    }
  }

  void worker_main(unsigned int index, std::promise<void> ready) {
    bool announced = false;
    try {
      wheelio::Reactor reactor(config.backend, config.queue_depth,
                               config.disk_threads);
      bool acceptor = index == 0;
      WorkerContext context{reactor,
                            file_fd,
                            acceptor ? listen_fd : -1,
                            config.multishot,
                            stopping,
                            handoff_in[index],
                            acceptor ? handoff_out : std::vector<int>(),
                            {}};
      if (acceptor) {
        wheelio::spawn(accept_loop(context));
      } else {
        wheelio::spawn(receive_loop(context));
      }
      ready.set_value();
      announced = true;

      while (!stopping.load()) {
        reactor.wait(stop_check_ms);
      }
      drain(context);
    } catch (...) {
      if (!announced) {
        ready.set_exception(std::current_exception());
      } else {
        fail(std::current_exception());
      }
    }
  }
};

Server::Server(ServerConfig config) : _impl(std::make_unique<Impl>()) {
  _impl->config = std::move(config);
}

Server::~Server() {
  try {
    stop();
  } catch (...) { // NOLINT: nowhere to report it from a destructor
  }
}

uint16_t Server::start() {
  Impl &impl = *_impl;
  if (impl.config.threads == 0) {
    throw std::invalid_argument("wheelie: at least one reactor is needed");
  }

  impl.file_fd = open_file(impl.config.file, impl.config.direct);
  try {
    impl.listen_fd = open_listener(impl.config.host, impl.config.port);
    uint16_t port = port_of(impl.listen_fd);

    impl.handoff_in.assign(impl.config.threads, -1);
    impl.handoff_out.assign(impl.config.threads, -1);
    for (unsigned int i = 1; i < impl.config.threads; ++i) {
      int pair[2];
      if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) {
        throw_errno(errno, "socketpair");
      }
      impl.handoff_out[i] = pair[0];
      impl.handoff_in[i] = pair[1];
    }

    std::vector<std::future<void>> ready;
    for (unsigned int i = 0; i < impl.config.threads; ++i) {
      std::promise<void> promise;
      ready.push_back(promise.get_future());
      impl.workers.emplace_back(&Impl::worker_main, &impl, i,
                                std::move(promise));
    }
    for (std::future<void> &future : ready) {
      future.get();
    }
    return port;
  } catch (...) {
    stop();
    throw;
  }
}

void Server::stop() {
  Impl &impl = *_impl;
  impl.stopping.store(true);
  for (std::thread &worker : impl.workers) {
    worker.join();
  }
  impl.workers.clear();

  // What the acceptor gave and no reactor took is closed.
  for (int fd : impl.handoff_in) {
    while (fd >= 0) {
      int given;
      if (::recv(fd, &given, sizeof(given), MSG_DONTWAIT) !=
          static_cast<ssize_t>(sizeof(given))) {
        break;
      }
      ::close(given);
    }
  }
  for (std::vector<int> *fds : {&impl.handoff_in, &impl.handoff_out}) {
    for (int &fd : *fds) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }

  if (impl.listen_fd >= 0) {
    ::close(impl.listen_fd);
    impl.listen_fd = -1;
  }
  if (impl.file_fd >= 0) {
    ::close(impl.file_fd);
    impl.file_fd = -1;
  }

  if (std::exception_ptr error = std::exchange(impl.error, nullptr)) {
    std::rethrow_exception(error);
  }
}

} // namespace wheelie
