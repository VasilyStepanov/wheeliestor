#ifndef WHEELIO_SRC_REACTOR_H
#define WHEELIO_SRC_REACTOR_H

#include <wheelio/wheelio.h>

#define WIO_RECV_MAX_ENTRIES 32768u

/*
 * What a backend does for each operation. The generic layer has already
 * checked the arguments, claimed a place in the reactor for `op` and stored
 * its callback; a backend takes it from there, keeping whatever it needs of
 * the request itself, and returns 0 or -errno. Only a returned 0 makes the
 * operation count as in flight.
 */
typedef struct WioReactorOps {
  void (*destroy)(WioReactor *reactor);
  int (*wait)(WioReactor *reactor, int timeout_ms);
  int (*cancel)(WioReactor *reactor, WioOp *target);
  int (*cancel_fd)(WioReactor *reactor, int fd);

  int (*accept)(WioReactor *reactor, WioOp *op, int fd, struct sockaddr *addr,
                socklen_t *addrlen);
  int (*connect)(WioReactor *reactor, WioOp *op, int fd,
                 const struct sockaddr *addr, socklen_t addrlen);
  int (*close)(WioReactor *reactor, WioOp *op, int fd);
  int (*recv)(WioReactor *reactor, WioOp *op, int fd, void *buf, size_t size,
              unsigned int flags);
  int (*recv_multishot)(WioReactor *reactor, WioOp *op, int fd,
                        size_t entry_size, unsigned int entries,
                        unsigned int flags);
  int (*send)(WioReactor *reactor, WioOp *op, int fd, const void *buf,
              size_t size, unsigned int flags);
  int (*sendmsg)(WioReactor *reactor, WioOp *op, int fd,
                 const struct msghdr *msg, unsigned int flags);
  int (*pread)(WioReactor *reactor, WioOp *op, int fd, void *buf, size_t size,
               uint64_t offset);
  int (*pwrite)(WioReactor *reactor, WioOp *op, int fd, const void *buf,
                size_t size, uint64_t offset, unsigned int flags);
  int (*fsync)(WioReactor *reactor, WioOp *op, int fd, bool datasync);
} WioReactorOps;

/* Common head of every backend's reactor. */
struct WioReactor {
  const WioReactorOps *ops;
  unsigned int depth;
  unsigned int inflight;
};

/*
 * Backends report completions through these. A completion without WIO_F_MORE
 * is the final one: the operation stops counting as in flight and its
 * memory belongs to the caller again before the callback runs.
 */
void wio_reactor_complete(WioOp *op, int result, unsigned int flags);
void wio_reactor_complete_recv(WioOp *op, const void *data, int result,
                               unsigned int flags);

int wio_uring_reactor_create(unsigned int depth, WioReactor **reactor);

#endif /* WHEELIO_SRC_REACTOR_H */
