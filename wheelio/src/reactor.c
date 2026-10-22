#include "reactor.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

int wio_reactor_create(WioBackend backend, unsigned int depth,
                       WioReactor **reactor) {
  if (depth == 0) {
    return -EINVAL;
  }
  switch (backend) {
  case WIO_BACKEND_URING:
    return wio_uring_reactor_create(depth, reactor);
  case WIO_BACKEND_LIBC:
  case WIO_BACKEND_ALIBC:
    return -ENOSYS;
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

/* Clears `op` for a new request of `kind`. */
static int begin(WioReactor *reactor, WioOp *op, unsigned int kind) {
  if (reactor->inflight >= reactor->depth) {
    return -EBUSY;
  }
  memset(op, 0, sizeof(*op));
  op->_reactor = reactor;
  op->_kind = kind;
  return 0;
}

static int finish(WioReactor *reactor, WioOp *op) {
  int ret = reactor->ops->submit(reactor, op);
  if (ret == 0) {
    ++reactor->inflight;
  }
  return ret;
}

/* Results are an int, so a transfer cannot be larger than that. */
static bool fits_result(size_t size) { return size <= INT_MAX; }

int wio_accept(WioReactor *reactor, WioOp *op, int fd, struct sockaddr *addr,
               socklen_t *addrlen, WioCallback cb) {
  int ret = begin(reactor, op, WIO_KIND_ACCEPT);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = addr;
  op->_u._accept.addrlen = addrlen;
  return finish(reactor, op);
}

int wio_connect(WioReactor *reactor, WioOp *op, int fd,
                const struct sockaddr *addr, socklen_t addrlen,
                WioCallback cb) {
  int ret = begin(reactor, op, WIO_KIND_CONNECT);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = (void *)addr;
  op->_size = addrlen;
  return finish(reactor, op);
}

int wio_close(WioReactor *reactor, WioOp *op, int fd, WioCallback cb) {
  int ret = begin(reactor, op, WIO_KIND_CLOSE);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  return finish(reactor, op);
}

int wio_recv(WioReactor *reactor, WioOp *op, int fd, void *buf, size_t size,
             unsigned int flags, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = begin(reactor, op, WIO_KIND_RECV);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = buf;
  op->_size = size;
  op->_flags = flags;
  return finish(reactor, op);
}

int wio_recv_multishot(WioReactor *reactor, WioOp *op, int fd,
                       size_t entry_size, unsigned int entries,
                       unsigned int flags, WioRecvCallback cb) {
  if (entry_size == 0 || !fits_result(entry_size) || entries == 0 ||
      entries > WIO_RECV_MAX_ENTRIES || (entries & (entries - 1)) != 0) {
    return -EINVAL;
  }
  int ret = begin(reactor, op, WIO_KIND_RECV_MULTISHOT);
  if (ret < 0) {
    return ret;
  }
  op->_cb.recv = cb;
  op->_fd = fd;
  op->_flags = flags;
  op->_u._recv_ms.entry_size = entry_size;
  op->_u._recv_ms.entries = entries;
  return finish(reactor, op);
}

int wio_send(WioReactor *reactor, WioOp *op, int fd, const void *buf,
             size_t size, unsigned int flags, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = begin(reactor, op, WIO_KIND_SEND);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = (void *)buf;
  op->_size = size;
  op->_flags = flags;
  return finish(reactor, op);
}

int wio_sendmsg(WioReactor *reactor, WioOp *op, int fd,
                const struct msghdr *msg, unsigned int flags, WioCallback cb) {
  int ret = begin(reactor, op, WIO_KIND_SENDMSG);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = (void *)msg;
  op->_flags = flags;
  return finish(reactor, op);
}

int wio_pread(WioReactor *reactor, WioOp *op, int fd, void *buf, size_t size,
              uint64_t offset, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = begin(reactor, op, WIO_KIND_PREAD);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = buf;
  op->_size = size;
  op->_offset = offset;
  return finish(reactor, op);
}

int wio_pwrite(WioReactor *reactor, WioOp *op, int fd, const void *buf,
               size_t size, uint64_t offset, unsigned int flags,
               WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = begin(reactor, op, WIO_KIND_PWRITE);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = (void *)buf;
  op->_size = size;
  op->_offset = offset;
  op->_flags = flags;
  return finish(reactor, op);
}

int wio_fsync(WioReactor *reactor, WioOp *op, int fd, bool datasync,
              WioCallback cb) {
  int ret = begin(reactor, op, WIO_KIND_FSYNC);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_flags = datasync;
  return finish(reactor, op);
}

int wio_cancel(WioReactor *reactor, WioOp *target) {
  return reactor->ops->cancel(reactor, target);
}

int wio_cancel_fd(WioReactor *reactor, int fd) {
  return reactor->ops->cancel_fd(reactor, fd);
}
