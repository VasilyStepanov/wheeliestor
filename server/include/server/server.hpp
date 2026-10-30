#ifndef WHEELIE_SERVER_SERVER_HPP
#define WHEELIE_SERVER_SERVER_HPP

#include <wheelio/wheelio.h>

#include <cstdint>
#include <memory>
#include <string>

namespace wheelie {

struct ServerConfig {
  std::string host = "0.0.0.0";
  // 0 lets the system choose; Server::start() returns the one it chose.
  uint16_t port = 0;
  // The file or block device that is served.
  std::string file;
  WioBackend backend = WIO_BACKEND_URING;
  // Receive with one multishot receive per connection instead of a receive
  // per chunk. The libc backends emulate it.
  bool multishot = false;
  // Threads, each with a reactor of its own, sharing the listening socket.
  unsigned int threads = 1;
  // Operations a reactor may have in flight: a connection keeps one receive
  // and, for each request it has not answered yet, a disk operation or a send.
  unsigned int queue_depth = 1024;
  // Disk threads of the pool of a reactor of the alibc backend; the other
  // backends have none.
  unsigned int disk_threads = WIO_ALIBC_THREADS;
  // Open the file with O_DIRECT.
  bool direct = true;
};

/**
 * Serves a file over the wheelie protocol: reads and writes at the offsets the
 * clients ask for.
 */
class Server final {
public:
  explicit Server(ServerConfig config);
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  ~Server();

  // Opens the file and the listening socket and starts the threads. Returns
  // the port. Throws std::system_error if any of that fails.
  uint16_t start();

  // Stops serving: connections are cancelled, and requests already begun are
  // given a moment to finish. Throws what a reactor thread threw, if one did.
  void stop();

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace wheelie

#endif // WHEELIE_SERVER_SERVER_HPP
