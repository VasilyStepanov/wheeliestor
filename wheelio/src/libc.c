#define _GNU_SOURCE

#include "pool.h"
#include "reactor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/*
 * Plain libc: sockets are driven with poll() and nonblocking calls. Disk I/O
 * is an ordinary blocking call, made either by the loop itself or, if the
 * reactor has a pool, by one of its worker threads.
 *
 * An operation is on exactly one list at a time:
 *  - submitted: recorded, not tried yet;
 *  - waiting: its descriptor was not ready, so it is parked on poll();
 *  - completed: its result is known and its callback has not run;
 *  - pool: a worker has it, and it is on no list of the reactor.
 * Callbacks run only after the syscalls of a wait are done, so they are free
 * to cancel and submit.
 *
 * An operation in the pool cannot be cancelled: its syscall is not
 * interrupted, and it completes with its own result.
 */

enum {
  LIST_NONE = 0,
  LIST_SUBMITTED,
  LIST_WAITING,
  LIST_COMPLETED,
  LIST_POOL,
  LIST_COUNT,
};

/* What to do with an operation once it is its turn. */
enum {
  OP_ACCEPT = 1,
  OP_CONNECT,
  OP_CONNECT_DONE,
  OP_CLOSE,
  OP_RECV,
  OP_RECV_MULTISHOT,
  OP_SEND,
  OP_SENDMSG,
  OP_PREAD,
  OP_PWRITE,
  OP_FSYNC,
};

typedef struct {
  WioOp *head;
  WioOp *tail;
  unsigned int count;
} List;

typedef struct {
  WioReactor base;
  List lists[LIST_COUNT];
  /* One slot per operation that can be waiting, and one for the pool. */
  struct pollfd *pfds;
  WioOp **pops;
  WioPool *pool;
  /* Operations handed to the pool and not yet taken back. */
  unsigned int pool_pending;
} LibcReactor;

#define RETRY(call)                                                            \
  ({                                                                           \
    ssize_t retry_result;                                                      \
    do {                                                                       \
      retry_result = (call);                                                   \
    } while (retry_result < 0 && errno == EINTR);                              \
    retry_result;                                                              \
  })

static LibcReactor *libc_reactor(WioReactor *reactor) {
  return (LibcReactor *)reactor;
}

static void list_push(LibcReactor *reactor, unsigned int which, WioOp *op) {
  List *list = &reactor->lists[which];
  op->_poll.list = which;
  op->_poll.next = NULL;
  op->_poll.prev = list->tail;
  if (list->tail != NULL) {
    list->tail->_poll.next = op;
  } else {
    list->head = op;
  }
  list->tail = op;
  ++list->count;
}

static void list_remove(LibcReactor *reactor, WioOp *op) {
  List *list = &reactor->lists[op->_poll.list];
  if (op->_poll.prev != NULL) {
    op->_poll.prev->_poll.next = op->_poll.next;
  } else {
    list->head = op->_poll.next;
  }
  if (op->_poll.next != NULL) {
    op->_poll.next->_poll.prev = op->_poll.prev;
  } else {
    list->tail = op->_poll.prev;
  }
  --list->count;
  op->_poll.list = LIST_NONE;
}

static WioOp *list_pop(LibcReactor *reactor, unsigned int which) {
  WioOp *op = reactor->lists[which].head;
  if (op != NULL) {
    list_remove(reactor, op);
  }
  return op;
}

static int ensure_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) {
    return -errno;
  }
  if (!(flags & O_NONBLOCK) && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -errno;
  }
  return 0;
}

static int errno_result(ssize_t ret) { return ret < 0 ? -errno : (int)ret; }

static bool would_block(ssize_t ret) {
  return ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static void park(LibcReactor *reactor, WioOp *op, short events) {
  op->_poll.events = events;
  list_push(reactor, LIST_WAITING, op);
}

static void finish(LibcReactor *reactor, WioOp *op, int result) {
  op->_poll.result = result;
  list_push(reactor, LIST_COMPLETED, op);
}

/* Records a request, to be tried by the next wait. */
static void record(LibcReactor *reactor, WioOp *op, unsigned int opcode,
                   int fd) {
  op->_poll.opcode = opcode;
  op->_poll.fd = fd;
  op->_poll.flags = 0;
  op->_poll.ptr = NULL;
  op->_poll.aux = NULL;
  op->_poll.size = 0;
  op->_poll.offset = 0;
  list_push(reactor, LIST_SUBMITTED, op);
}

static int libc_accept(WioReactor *base, WioOp *op, int fd,
                       struct sockaddr *addr, socklen_t *addrlen) {
  int ret = ensure_nonblocking(fd);
  if (ret < 0) {
    return ret;
  }
  record(libc_reactor(base), op, OP_ACCEPT, fd);
  op->_poll.ptr = addr;
  op->_poll.aux = addrlen;
  return 0;
}

static int libc_connect(WioReactor *base, WioOp *op, int fd,
                        const struct sockaddr *addr, socklen_t addrlen) {
  int ret = ensure_nonblocking(fd);
  if (ret < 0) {
    return ret;
  }
  record(libc_reactor(base), op, OP_CONNECT, fd);
  op->_poll.ptr = (void *)addr;
  op->_poll.size = addrlen;
  return 0;
}

static int libc_close(WioReactor *base, WioOp *op, int fd) {
  record(libc_reactor(base), op, OP_CLOSE, fd);
  return 0;
}

static int libc_recv(WioReactor *base, WioOp *op, int fd, void *buf,
                     size_t size, unsigned int flags) {
  record(libc_reactor(base), op, OP_RECV, fd);
  op->_poll.ptr = buf;
  op->_poll.size = size;
  op->_poll.flags = flags;
  return 0;
}

/* One buffer is enough here: a chunk is handed over and gone before the next
 * one is received, so `entries` buffers are not needed. */
static int libc_recv_multishot(WioReactor *base, WioOp *op, int fd,
                               size_t entry_size, unsigned int entries,
                               unsigned int flags) {
  (void)entries;
  void *buf = malloc(entry_size);
  if (buf == NULL) {
    return -ENOMEM;
  }
  record(libc_reactor(base), op, OP_RECV_MULTISHOT, fd);
  op->_poll.ptr = buf;
  op->_poll.size = entry_size;
  op->_poll.flags = flags;
  return 0;
}

static int libc_send(WioReactor *base, WioOp *op, int fd, const void *buf,
                     size_t size, unsigned int flags) {
  record(libc_reactor(base), op, OP_SEND, fd);
  op->_poll.ptr = (void *)buf;
  op->_poll.size = size;
  op->_poll.flags = flags;
  return 0;
}

static int libc_sendmsg(WioReactor *base, WioOp *op, int fd,
                        const struct msghdr *msg, unsigned int flags) {
  record(libc_reactor(base), op, OP_SENDMSG, fd);
  op->_poll.ptr = (void *)msg;
  op->_poll.flags = flags;
  return 0;
}

static int libc_pread(WioReactor *base, WioOp *op, int fd, void *buf,
                      size_t size, uint64_t offset) {
  record(libc_reactor(base), op, OP_PREAD, fd);
  op->_poll.ptr = buf;
  op->_poll.size = size;
  op->_poll.offset = offset;
  return 0;
}

static int libc_pwrite(WioReactor *base, WioOp *op, int fd, const void *buf,
                       size_t size, uint64_t offset, unsigned int flags) {
  record(libc_reactor(base), op, OP_PWRITE, fd);
  op->_poll.ptr = (void *)buf;
  op->_poll.size = size;
  op->_poll.offset = offset;
  op->_poll.flags = flags;
  return 0;
}

static int libc_fsync(WioReactor *base, WioOp *op, int fd, bool datasync) {
  record(libc_reactor(base), op, OP_FSYNC, fd);
  op->_poll.flags = datasync;
  return 0;
}

/* Cancels `op` if it has not got a result yet; it then completes with
 * -ECANCELED. */
static void cancel_op(LibcReactor *reactor, WioOp *op) {
  op->_cancelled = true;
  if (op->_poll.list == LIST_SUBMITTED || op->_poll.list == LIST_WAITING) {
    list_remove(reactor, op);
    finish(reactor, op, -ECANCELED);
  }
}

static int libc_cancel(WioReactor *base, WioOp *target) {
  cancel_op(libc_reactor(base), target);
  return 0;
}

static int libc_cancel_fd(WioReactor *base, int fd) {
  LibcReactor *reactor = libc_reactor(base);
  for (unsigned int which = LIST_SUBMITTED; which <= LIST_WAITING; ++which) {
    WioOp *op = reactor->lists[which].head;
    while (op != NULL) {
      WioOp *next = op->_poll.next;
      if (op->_poll.fd == fd) {
        cancel_op(reactor, op);
      }
      op = next;
    }
  }
  return 0;
}

/* The blocking calls on a file, the part that may take a long time. */
static int disk_op(const WioOp *op) {
  int fd = op->_poll.fd;
  ssize_t ret;

  switch (op->_poll.opcode) {
  case OP_PREAD:
    ret = RETRY(
        pread(fd, op->_poll.ptr, op->_poll.size, (off_t)op->_poll.offset));
    break;
  case OP_PWRITE:
    if (op->_poll.flags & WIO_WRITE_DSYNC) {
      struct iovec iov = {op->_poll.ptr, op->_poll.size};
      ret = RETRY(pwritev2(fd, &iov, 1, (off_t)op->_poll.offset, RWF_DSYNC));
    } else {
      ret = RETRY(
          pwrite(fd, op->_poll.ptr, op->_poll.size, (off_t)op->_poll.offset));
    }
    break;
  default:
    ret = RETRY(op->_poll.flags ? fdatasync(fd) : fsync(fd));
    break;
  }
  return errno_result(ret);
}

static void pool_work(WioOp *op) { op->_poll.result = disk_op(op); }

/*
 * Does what `op` asks for. It either gets a result, or, for a socket that is
 * not ready, is parked until it is. `op` is on no list.
 */
static void run(LibcReactor *reactor, WioOp *op) {
  int fd = op->_poll.fd;
  ssize_t ret;

  switch (op->_poll.opcode) {
  case OP_ACCEPT:
    ret = RETRY(accept4(fd, op->_poll.ptr, op->_poll.aux, SOCK_CLOEXEC));
    if (would_block(ret)) {
      park(reactor, op, POLLIN);
      return;
    }
    break;
  case OP_CONNECT:
    ret = connect(fd, op->_poll.ptr, (socklen_t)op->_poll.size);
    if (ret < 0 && (errno == EINPROGRESS || errno == EINTR)) {
      op->_poll.opcode = OP_CONNECT_DONE;
      park(reactor, op, POLLOUT);
      return;
    }
    break;
  case OP_CONNECT_DONE: {
    int error = 0;
    socklen_t length = sizeof(error);
    ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
    if (ret == 0) {
      finish(reactor, op, -error);
      return;
    }
    break;
  }
  case OP_CLOSE:
    ret = close(fd);
    break;
  case OP_RECV:
  case OP_RECV_MULTISHOT:
    ret = RETRY(recv(fd, op->_poll.ptr, op->_poll.size,
                     (int)op->_poll.flags | MSG_DONTWAIT));
    if (would_block(ret)) {
      park(reactor, op, POLLIN);
      return;
    }
    break;
  case OP_SEND:
    ret = RETRY(send(fd, op->_poll.ptr, op->_poll.size,
                     (int)op->_poll.flags | MSG_NOSIGNAL | MSG_DONTWAIT));
    if (would_block(ret)) {
      park(reactor, op, POLLOUT);
      return;
    }
    break;
  case OP_SENDMSG:
    ret = RETRY(sendmsg(fd, op->_poll.ptr,
                        (int)op->_poll.flags | MSG_NOSIGNAL | MSG_DONTWAIT));
    if (would_block(ret)) {
      park(reactor, op, POLLOUT);
      return;
    }
    break;
  case OP_PREAD:
  case OP_PWRITE:
  case OP_FSYNC:
    if (reactor->pool != NULL) {
      op->_poll.list = LIST_POOL;
      ++reactor->pool_pending;
      wio_pool_submit(reactor->pool, op);
    } else {
      finish(reactor, op, disk_op(op));
    }
    return;
  default:
    ret = -1;
    errno = EINVAL;
    break;
  }
  finish(reactor, op, errno_result(ret));
}

/* Runs the callback of a multishot receive that has a result. */
static int deliver_recv_multishot(LibcReactor *reactor, WioOp *op) {
  int result = op->_poll.result;
  int callbacks = 0;

  if (result > 0) {
    wio_reactor_complete_recv(op, op->_poll.ptr, result, WIO_F_MORE);
    callbacks = 1;
    if (!op->_cancelled) {
      list_push(reactor, LIST_SUBMITTED, op);
      return callbacks;
    }
    result = -ECANCELED;
    ++callbacks;
  } else {
    callbacks = 1;
  }

  /* The buffer goes before the callback, which is free to release `op`. */
  free(op->_poll.ptr);
  op->_poll.ptr = NULL;
  wio_reactor_complete_recv(op, NULL, result, 0);
  return callbacks;
}

static int deliver(LibcReactor *reactor) {
  int callbacks = 0;
  WioOp *op;

  while ((op = list_pop(reactor, LIST_COMPLETED)) != NULL) {
    if (op->_poll.opcode == OP_RECV_MULTISHOT) {
      callbacks += deliver_recv_multishot(reactor, op);
    } else {
      wio_reactor_complete(op, op->_poll.result, 0);
      ++callbacks;
    }
  }
  return callbacks;
}

/* Takes the operations the workers have finished. */
static void reap(LibcReactor *reactor) {
  WioOp *op = wio_pool_take_done(reactor->pool);
  while (op != NULL) {
    WioOp *next = op->_poll.next;
    --reactor->pool_pending;
    list_push(reactor, LIST_COMPLETED, op);
    op = next;
  }
}

static int libc_wait(WioReactor *base, int timeout_ms) {
  LibcReactor *reactor = libc_reactor(base);
  WioOp *op;

  /* Everything recorded gets its first try, as it would in code that calls
   * the syscall first and polls only if it would block. */
  while ((op = list_pop(reactor, LIST_SUBMITTED)) != NULL) {
    run(reactor, op);
  }

  if (reactor->pool_pending > 0) {
    reap(reactor);
  }

  /* Whatever is still in the pool is waited for through its pipe. */
  bool pool_wake = reactor->pool_pending > 0;
  List *waiting = &reactor->lists[LIST_WAITING];
  if (waiting->count > 0 || pool_wake) {
    unsigned int n = 0;
    for (op = waiting->head; op != NULL; op = op->_poll.next, ++n) {
      reactor->pfds[n].fd = op->_poll.fd;
      reactor->pfds[n].events = op->_poll.events;
      reactor->pfds[n].revents = 0;
      reactor->pops[n] = op;
    }
    unsigned int total = n;
    if (pool_wake) {
      reactor->pfds[total].fd = wio_pool_wake_fd(reactor->pool);
      reactor->pfds[total].events = POLLIN;
      reactor->pfds[total].revents = 0;
      ++total;
    }

    /* Results already in hand are not worth waiting for more. */
    int poll_timeout =
        reactor->lists[LIST_COMPLETED].count > 0 ? 0 : timeout_ms;
    int ready = poll(reactor->pfds, total, poll_timeout);
    if (ready < 0 && errno != EINTR) {
      return -errno;
    }
    for (unsigned int i = 0; ready > 0 && i < n; ++i) {
      if (reactor->pfds[i].revents != 0) {
        op = reactor->pops[i];
        list_remove(reactor, op);
        run(reactor, op);
      }
    }
    if (pool_wake && ready > 0 && reactor->pfds[n].revents != 0) {
      reap(reactor);
    }
  }

  return deliver(reactor);
}

static void libc_destroy(WioReactor *base) {
  LibcReactor *reactor = libc_reactor(base);
  if (reactor->pool != NULL) {
    wio_pool_destroy(reactor->pool);
  }
  free(reactor->pfds);
  free(reactor->pops);
  free(reactor);
}

static const WioReactorOps libc_ops = {
    .destroy = libc_destroy,
    .wait = libc_wait,
    .cancel = libc_cancel,
    .cancel_fd = libc_cancel_fd,
    .accept = libc_accept,
    .connect = libc_connect,
    .close = libc_close,
    .recv = libc_recv,
    .recv_multishot = libc_recv_multishot,
    .send = libc_send,
    .sendmsg = libc_sendmsg,
    .pread = libc_pread,
    .pwrite = libc_pwrite,
    .fsync = libc_fsync,
};

int wio_libc_reactor_create(unsigned int depth, unsigned int threads,
                            WioReactor **out) {
  LibcReactor *reactor = calloc(1, sizeof(*reactor));
  if (reactor == NULL) {
    return -ENOMEM;
  }
  reactor->pfds = calloc(depth + 1, sizeof(*reactor->pfds));
  reactor->pops = calloc(depth, sizeof(*reactor->pops));
  if (reactor->pfds == NULL || reactor->pops == NULL) {
    libc_destroy(&reactor->base);
    return -ENOMEM;
  }
  if (threads > 0) {
    int ret = wio_pool_create(threads, pool_work, &reactor->pool);
    if (ret < 0) {
      libc_destroy(&reactor->base);
      return ret;
    }
  }

  reactor->base.ops = &libc_ops;
  reactor->base.depth = depth;
  *out = &reactor->base;
  return 0;
}
