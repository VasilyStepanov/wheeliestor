#ifndef WHEELIO_TESTS_FD_HPP
#define WHEELIO_TESTS_FD_HPP

#include <sys/socket.h>
#include <unistd.h>

// Closes the descriptor it holds, if any.
class Fd {
public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  ~Fd() { reset(); }

  int get() const { return fd_; }

  void reset() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void reset(int fd) {
    reset();
    fd_ = fd;
  }

private:
  int fd_ = -1;
};

// Two connected stream sockets.
inline bool socket_pair(Fd &a, Fd &b) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return false;
  }
  a.reset(fds[0]);
  b.reset(fds[1]);
  return true;
}

#endif // WHEELIO_TESTS_FD_HPP
