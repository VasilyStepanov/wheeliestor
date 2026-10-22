#include "queue.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

int wio_queue_create(WioBackend backend, unsigned int depth, WioQueue **queue) {
  if (depth == 0) {
    return -EINVAL;
  }
  switch (backend) {
  case WIO_BACKEND_URING:
    return wio_uring_queue_create(depth, queue);
  case WIO_BACKEND_LIBC:
  case WIO_BACKEND_ALIBC:
    return -ENOSYS;
  }
  return -EINVAL;
}

void wio_queue_delete(WioQueue *queue) {
  if (queue != NULL) {
    queue->ops->destroy(queue);
  }
}

unsigned int wio_queue_inflight(const WioQueue *queue) {
  return queue->inflight;
}

int wio_wait(WioQueue *queue, int timeout_ms) {
  if (queue->inflight == 0) {
    return 0;
  }
  return queue->ops->wait(queue, timeout_ms);
}

void wio_queue_complete(WioOp *op, int result, unsigned int flags) {
  if (!(flags & WIO_F_MORE)) {
    --op->_queue->inflight;
  }
  op->_cb.op(op, result, flags);
}

void wio_queue_complete_recv(WioOp *op, const void *data, int result,
                             unsigned int flags) {
  if (!(flags & WIO_F_MORE)) {
    --op->_queue->inflight;
  }
  op->_cb.recv(op, data, result, flags);
}

/* Clears `op` for a new request of `kind`. */
static int begin(WioQueue *queue, WioOp *op, unsigned int kind) {
  if (queue->inflight >= queue->depth) {
    return -EBUSY;
  }
  memset(op, 0, sizeof(*op));
  op->_queue = queue;
  op->_kind = kind;
  return 0;
}

static int finish(WioQueue *queue, WioOp *op) {
  int ret = queue->ops->submit(queue, op);
  if (ret == 0) {
    ++queue->inflight;
  }
  return ret;
}

/* Results are an int, so a transfer cannot be larger than that. */
static bool fits_result(size_t size) { return size <= INT_MAX; }

int wio_accept(WioQueue *queue, WioOp *op, int fd, struct sockaddr *addr,
               socklen_t *addrlen, WioCallback cb) {
  int ret = begin(queue, op, WIO_KIND_ACCEPT);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = addr;
  op->_u._accept.addrlen = addrlen;
  return finish(queue, op);
}

int wio_connect(WioQueue *queue, WioOp *op, int fd, const struct sockaddr *addr,
                socklen_t addrlen, WioCallback cb) {
  int ret = begin(queue, op, WIO_KIND_CONNECT);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = (void *)addr;
  op->_size = addrlen;
  return finish(queue, op);
}

int wio_close(WioQueue *queue, WioOp *op, int fd, WioCallback cb) {
  int ret = begin(queue, op, WIO_KIND_CLOSE);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  return finish(queue, op);
}

int wio_recv(WioQueue *queue, WioOp *op, int fd, void *buf, size_t size,
             unsigned int flags, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = begin(queue, op, WIO_KIND_RECV);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = buf;
  op->_size = size;
  op->_flags = flags;
  return finish(queue, op);
}

int wio_recv_multishot(WioQueue *queue, WioOp *op, int fd, size_t entry_size,
                       unsigned int entries, unsigned int flags,
                       WioRecvCallback cb) {
  if (entry_size == 0 || !fits_result(entry_size) || entries == 0 ||
      entries > WIO_RECV_MAX_ENTRIES || (entries & (entries - 1)) != 0) {
    return -EINVAL;
  }
  int ret = begin(queue, op, WIO_KIND_RECV_MULTISHOT);
  if (ret < 0) {
    return ret;
  }
  op->_cb.recv = cb;
  op->_fd = fd;
  op->_flags = flags;
  op->_u._recv_ms.entry_size = entry_size;
  op->_u._recv_ms.entries = entries;
  return finish(queue, op);
}

int wio_send(WioQueue *queue, WioOp *op, int fd, const void *buf, size_t size,
             unsigned int flags, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = begin(queue, op, WIO_KIND_SEND);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = (void *)buf;
  op->_size = size;
  op->_flags = flags;
  return finish(queue, op);
}

int wio_sendmsg(WioQueue *queue, WioOp *op, int fd, const struct msghdr *msg,
                unsigned int flags, WioCallback cb) {
  int ret = begin(queue, op, WIO_KIND_SENDMSG);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = (void *)msg;
  op->_flags = flags;
  return finish(queue, op);
}

int wio_pread(WioQueue *queue, WioOp *op, int fd, void *buf, size_t size,
              uint64_t offset, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = begin(queue, op, WIO_KIND_PREAD);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = buf;
  op->_size = size;
  op->_offset = offset;
  return finish(queue, op);
}

int wio_pwrite(WioQueue *queue, WioOp *op, int fd, const void *buf, size_t size,
               uint64_t offset, unsigned int flags, WioCallback cb) {
  if (!fits_result(size)) {
    return -EINVAL;
  }
  int ret = begin(queue, op, WIO_KIND_PWRITE);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_ptr = (void *)buf;
  op->_size = size;
  op->_offset = offset;
  op->_flags = flags;
  return finish(queue, op);
}

int wio_fsync(WioQueue *queue, WioOp *op, int fd, bool datasync,
              WioCallback cb) {
  int ret = begin(queue, op, WIO_KIND_FSYNC);
  if (ret < 0) {
    return ret;
  }
  op->_cb.op = cb;
  op->_fd = fd;
  op->_flags = datasync;
  return finish(queue, op);
}

int wio_cancel(WioQueue *queue, WioOp *target) {
  return queue->ops->cancel(queue, target);
}

int wio_cancel_fd(WioQueue *queue, int fd) {
  return queue->ops->cancel_fd(queue, fd);
}
