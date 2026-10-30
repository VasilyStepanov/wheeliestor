#include "reactor.h"

#include <errno.h>
#include <limits.h>

int wio_reactor_create(WioBackend backend, unsigned int depth,
                       WioReactor **reactor) {
  return wio_reactor_create_pool(backend, depth, WIO_ALIBC_THREADS, reactor);
}

int wio_reactor_create_pool(WioBackend backend, unsigned int depth,
                            unsigned int threads, WioReactor **reactor) {
  if (depth == 0 || threads == 0) {
    return -EINVAL;
  }
  switch (backend) {
  case WIO_BACKEND_URING:
    return wio_uring_reactor_create(depth, reactor);
  case WIO_BACKEND_LIBC:
    return wio_libc_reactor_create(depth, 0, reactor);
  case WIO_BACKEND_ALIBC:
    return wio_libc_reactor_create(depth, threads, reactor);
  }
  return -EINVAL;
}

void wio_reactor_delete(WioReactor *reactor) {
  if (reactor != NULL) {
    reactor->ops->destroy(reactor);
  }
}

unsigned int wio_reactor_inflight(const WioReactor *reactor) {
  return reactor->inflight;
}

int wio_wait(WioReactor *reactor, int timeout_ms) {
  if (reactor->inflight == 0) {
    return 0;
  }
  return reactor->ops->wait(reactor, timeout_ms);
}

void wio_reactor_complete(WioOp *op, int result, unsigned int flags) {
  if (!(flags & WIO_F_MORE)) {
    --op->_reactor->inflight;
  }
  op->_cb.op(op, result, flags);
}

void wio_reactor_complete_recv(WioOp *op, const void *data, int result,
                               unsigned int flags) {
  if (!(flags & WIO_F_MORE)) {
    --op->_reactor->inflight;
  }
  op->_cb.recv(op, data, result, flags);
}

/* Claims a place for `op` in `reactor`, with nothing left of its past. */
static int claim(WioReactor *reactor, WioOp *op) {
  if (reactor->inflight >= reactor->depth) {
    return -EBUSY;
  }
  op->_reactor = reactor;
  op->_cancelled = false;
  op->_backend = NULL;
  return 0;
}

/* Counts the operation as in flight once a backend has taken it. */
static int started(WioReactor *reactor, int ret) {
  if (ret == 0) {
    ++reactor->inflight;
  }
  return ret;
}

/* Results are an int, so a transfer cannot be larger than that. */
static bool fits_result(size_t size) { return size <= INT_MAX; }

int wio_accept(WioReactor *reactor, WioOp *op, int fd, struct sockaddr *addr,
               socklen_t *addrlen, WioCallback cb) {
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(reactor, reactor->ops->accept(reactor, op, fd, addr, addrlen));
}

int wio_connect(WioReactor *reactor, WioOp *op, int fd,
                const struct sockaddr *addr, socklen_t addrlen,
                WioCallback cb) {
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(reactor,
                 reactor->ops->connect(reactor, op, fd, addr, addrlen));
}

int wio_close(WioReactor *reactor, WioOp *op, int fd, WioCallback cb) {
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(reactor, reactor->ops->close(reactor, op, fd));
}

int wio_recv(WioReactor *reactor, WioOp *op, int fd, void *buf, size_t size,
             unsigned int flags, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(reactor,
                 reactor->ops->recv(reactor, op, fd, buf, size, flags));
}

int wio_recv_multishot(WioReactor *reactor, WioOp *op, int fd,
                       size_t entry_size, unsigned int entries,
                       unsigned int flags, WioRecvCallback cb) {
  if (entry_size == 0 || !fits_result(entry_size) || entries == 0 ||
      entries > WIO_RECV_MAX_ENTRIES || (entries & (entries - 1)) != 0) {
    return -EINVAL;
  }
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.recv = cb;
  return started(reactor, reactor->ops->recv_multishot(
                              reactor, op, fd, entry_size, entries, flags));
}

int wio_send(WioReactor *reactor, WioOp *op, int fd, const void *buf,
             size_t size, unsigned int flags, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(reactor,
                 reactor->ops->send(reactor, op, fd, buf, size, flags));
}

int wio_sendmsg(WioReactor *reactor, WioOp *op, int fd,
                const struct msghdr *msg, unsigned int flags, WioCallback cb) {
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(reactor, reactor->ops->sendmsg(reactor, op, fd, msg, flags));
}

int wio_pread(WioReactor *reactor, WioOp *op, int fd, void *buf, size_t size,
              uint64_t offset, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(reactor,
                 reactor->ops->pread(reactor, op, fd, buf, size, offset));
}

int wio_pwrite(WioReactor *reactor, WioOp *op, int fd, const void *buf,
               size_t size, uint64_t offset, unsigned int flags,
               WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(
      reactor, reactor->ops->pwrite(reactor, op, fd, buf, size, offset, flags));
}

int wio_fsync(WioReactor *reactor, WioOp *op, int fd, bool datasync,
              WioCallback cb) {
  int ret = claim(reactor, op);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  return started(reactor, reactor->ops->fsync(reactor, op, fd, datasync));
}

int wio_cancel(WioReactor *reactor, WioOp *target) {
  return reactor->ops->cancel(reactor, target);
}

int wio_cancel_fd(WioReactor *reactor, int fd) {
  return reactor->ops->cancel_fd(reactor, fd);
}
