#include "reactor.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <liburing.h>
#include <linux/fs.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

/* Buffer group ids are 16 bits; a free-id bitmap keeps one unique per live
 * multishot receive however many connections come and go. */
#define BGID_COUNT 65536u

#define MAX_SQ_ENTRIES 32768u
#define CQ_ENTRIES_PER_SQ 4u
#define MAX_CQ_ENTRIES 65536u

typedef struct {
  WioReactor base;
  struct io_uring ring;
  uint64_t bgid_used[BGID_COUNT / 64];
} UringReactor;

/* Provided-buffer ring of one multishot receive: the descriptors the kernel
 * picks buffers from, followed by the buffers themselves, in one mapping. */
typedef struct {
  void *map;
  size_t map_len;
  struct io_uring_buf_ring *ring;
  char *data;
  size_t entry_size;
  unsigned int entries;
  int mask;
  unsigned short bgid;
} RecvRing;

static UringReactor *uring_reactor(WioReactor *reactor) {
  return (UringReactor *)reactor;
}

static int bgid_alloc(UringReactor *reactor) {
  for (unsigned int word = 0; word < BGID_COUNT / 64; ++word) {
    uint64_t free_bits = ~reactor->bgid_used[word];
    if (free_bits != 0) {
      unsigned int bit = (unsigned int)__builtin_ctzll(free_bits);
      reactor->bgid_used[word] |= UINT64_C(1) << bit;
      return (int)(word * 64 + bit);
    }
  }
  return -ENOSPC;
}

static void bgid_free(UringReactor *reactor, unsigned short bgid) {
  reactor->bgid_used[bgid / 64] &= ~(UINT64_C(1) << (bgid % 64));
}

static struct io_uring_sqe *get_sqe(UringReactor *reactor) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&reactor->ring);
  if (sqe != NULL) {
    return sqe;
  }
  /* The SQ ring is full of operations not yet handed to the kernel: hand
   * them over to make room. */
  if (io_uring_submit(&reactor->ring) < 0) {
    return NULL;
  }
  return io_uring_get_sqe(&reactor->ring);
}

static int recv_ring_create(UringReactor *reactor, WioOp *op) {
  size_t entry_size = op->_u._recv_ms.entry_size;
  unsigned int entries = op->_u._recv_ms.entries;

  RecvRing *recv_ring = calloc(1, sizeof(*recv_ring));
  if (recv_ring == NULL) {
    return -ENOMEM;
  }

  int bgid = bgid_alloc(reactor);
  if (bgid < 0) {
    free(recv_ring);
    return bgid;
  }

  size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  size_t descriptors_len =
      ((size_t)entries * sizeof(struct io_uring_buf) + page_size - 1) &
      ~(page_size - 1);
  recv_ring->map_len = descriptors_len + entry_size * entries;
  recv_ring->map = mmap(NULL, recv_ring->map_len, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (recv_ring->map == MAP_FAILED) {
    int ret = -errno;
    bgid_free(reactor, (unsigned short)bgid);
    free(recv_ring);
    return ret;
  }
  recv_ring->ring = recv_ring->map;
  recv_ring->data = (char *)recv_ring->map + descriptors_len;
  recv_ring->entry_size = entry_size;
  recv_ring->entries = entries;
  recv_ring->mask = io_uring_buf_ring_mask(entries);
  recv_ring->bgid = (unsigned short)bgid;

  io_uring_buf_ring_init(recv_ring->ring);
  struct io_uring_buf_reg reg;
  memset(&reg, 0, sizeof(reg));
  reg.ring_addr = (uint64_t)(uintptr_t)recv_ring->ring;
  reg.ring_entries = entries;
  reg.bgid = recv_ring->bgid;
  int ret = io_uring_register_buf_ring(&reactor->ring, &reg, 0);
  if (ret < 0) {
    munmap(recv_ring->map, recv_ring->map_len);
    bgid_free(reactor, recv_ring->bgid);
    free(recv_ring);
    return ret;
  }

  for (unsigned int i = 0; i < entries; ++i) {
    io_uring_buf_ring_add(recv_ring->ring, recv_ring->data + i * entry_size,
                          (unsigned int)entry_size, (unsigned short)i,
                          recv_ring->mask, (int)i);
  }
  io_uring_buf_ring_advance(recv_ring->ring, (int)entries);

  op->_backend = recv_ring;
  return 0;
}

static void recv_ring_destroy(UringReactor *reactor, WioOp *op) {
  RecvRing *recv_ring = op->_backend;
  io_uring_unregister_buf_ring(&reactor->ring, recv_ring->bgid);
  munmap(recv_ring->map, recv_ring->map_len);
  bgid_free(reactor, recv_ring->bgid);
  free(recv_ring);
  op->_backend = NULL;
}

/* Puts the receive of a multishot operation into the SQ ring. */
static int arm_recv(UringReactor *reactor, WioOp *op) {
  RecvRing *recv_ring = op->_backend;
  struct io_uring_sqe *sqe = get_sqe(reactor);
  if (sqe == NULL) {
    return -EBUSY;
  }
  io_uring_prep_recv_multishot(sqe, op->_fd, NULL, 0, (int)op->_flags);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = recv_ring->bgid;
  io_uring_sqe_set_data(sqe, op);
  return 0;
}

static int uring_submit(WioReactor *base, WioOp *op) {
  UringReactor *reactor = uring_reactor(base);

  if (op->_kind == WIO_KIND_RECV_MULTISHOT) {
    int ret = recv_ring_create(reactor, op);
    if (ret < 0) {
      return ret;
    }
    ret = arm_recv(reactor, op);
    if (ret < 0) {
      recv_ring_destroy(reactor, op);
    }
    return ret;
  }

  struct io_uring_sqe *sqe = get_sqe(reactor);
  if (sqe == NULL) {
    return -EBUSY;
  }

  switch (op->_kind) {
  case WIO_KIND_ACCEPT:
    io_uring_prep_accept(sqe, op->_fd, op->_ptr, op->_u._accept.addrlen,
                         SOCK_CLOEXEC);
    break;
  case WIO_KIND_CONNECT:
    io_uring_prep_connect(sqe, op->_fd, op->_ptr, (socklen_t)op->_size);
    break;
  case WIO_KIND_CLOSE:
    io_uring_prep_close(sqe, op->_fd);
    break;
  case WIO_KIND_RECV:
    io_uring_prep_recv(sqe, op->_fd, op->_ptr, op->_size, (int)op->_flags);
    break;
  case WIO_KIND_SEND:
    io_uring_prep_send(sqe, op->_fd, op->_ptr, op->_size,
                       (int)op->_flags | MSG_NOSIGNAL);
    break;
  case WIO_KIND_SENDMSG:
    io_uring_prep_sendmsg(sqe, op->_fd, op->_ptr, op->_flags | MSG_NOSIGNAL);
    break;
  case WIO_KIND_PREAD:
    io_uring_prep_read(sqe, op->_fd, op->_ptr, (unsigned int)op->_size,
                       op->_offset);
    break;
  case WIO_KIND_PWRITE:
    io_uring_prep_write(sqe, op->_fd, op->_ptr, (unsigned int)op->_size,
                        op->_offset);
    if (op->_flags & WIO_WRITE_DSYNC) {
      sqe->rw_flags |= RWF_DSYNC;
    }
    break;
  case WIO_KIND_FSYNC:
    io_uring_prep_fsync(sqe, op->_fd, op->_flags ? IORING_FSYNC_DATASYNC : 0);
    break;
  default:
    return -EINVAL;
  }
  io_uring_sqe_set_data(sqe, op);
  return 0;
}

static int uring_cancel(WioReactor *base, WioOp *target) {
  UringReactor *reactor = uring_reactor(base);
  struct io_uring_sqe *sqe = get_sqe(reactor);
  if (sqe == NULL) {
    return -EBUSY;
  }
  /* Remembered on the operation too: a multishot receive that is between two
   * arms right now has no request in the kernel for this to find. */
  target->_cancelled = true;
  io_uring_prep_cancel64(sqe, (uint64_t)(uintptr_t)target, 0);
  io_uring_sqe_set_data(sqe, NULL);
  return 0;
}

static int uring_cancel_fd(WioReactor *base, int fd) {
  UringReactor *reactor = uring_reactor(base);
  struct io_uring_sqe *sqe = get_sqe(reactor);
  if (sqe == NULL) {
    return -EBUSY;
  }
  io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
  io_uring_sqe_set_data(sqe, NULL);
  return 0;
}

/* Finishes a multishot receive: its ring goes away before the callback,
 * which is free to release the operation. */
static void recv_finish(UringReactor *reactor, WioOp *op, int result) {
  recv_ring_destroy(reactor, op);
  wio_reactor_complete_recv(op, NULL, result, 0);
}

static int complete_recv_multishot(UringReactor *reactor, WioOp *op, int res,
                                   unsigned int cqe_flags) {
  RecvRing *recv_ring = op->_backend;
  bool armed = cqe_flags & IORING_CQE_F_MORE;
  int callbacks = 0;

  if (res > 0 && (cqe_flags & IORING_CQE_F_BUFFER)) {
    unsigned int bid = cqe_flags >> IORING_CQE_BUFFER_SHIFT;
    const char *data = recv_ring->data + (size_t)bid * recv_ring->entry_size;
    wio_reactor_complete_recv(op, data, res, WIO_F_MORE);
    callbacks = 1;
    io_uring_buf_ring_add(recv_ring->ring, (void *)data,
                          (unsigned int)recv_ring->entry_size,
                          (unsigned short)bid, recv_ring->mask, 0);
    io_uring_buf_ring_advance(recv_ring->ring, 1);
  } else if (res == -ENOBUFS && !armed) {
    /* The kernel ran out of buffers and ended the receive; the buffers of
     * the completions already seen are back in the ring by now. */
  } else {
    recv_finish(reactor, op, res);
    return 1;
  }

  if (!armed) {
    if (op->_cancelled) {
      recv_finish(reactor, op, -ECANCELED);
      return callbacks + 1;
    }
    int ret = arm_recv(reactor, op);
    if (ret < 0) {
      recv_finish(reactor, op, ret);
      return callbacks + 1;
    }
  }
  return callbacks;
}

static int dispatch(UringReactor *reactor) {
  unsigned int head;
  unsigned int seen = 0;
  int callbacks = 0;
  struct io_uring_cqe *cqe;

  io_uring_for_each_cqe(&reactor->ring, head, cqe) {
    ++seen;
    WioOp *op = io_uring_cqe_get_data(cqe);
    if (op == NULL) {
      continue;
    }
    if (op->_kind == WIO_KIND_RECV_MULTISHOT) {
      callbacks += complete_recv_multishot(reactor, op, cqe->res, cqe->flags);
    } else {
      wio_reactor_complete(op, cqe->res, 0);
      ++callbacks;
    }
  }
  io_uring_cq_advance(&reactor->ring, seen);
  return callbacks;
}

static int uring_wait(WioReactor *base, int timeout_ms) {
  UringReactor *reactor = uring_reactor(base);
  int ret;

  if (timeout_ms < 0) {
    ret = io_uring_submit_and_wait(&reactor->ring, 1);
  } else if (timeout_ms == 0) {
    ret = io_uring_submit_and_get_events(&reactor->ring);
  } else {
    struct io_uring_cqe *cqe;
    struct __kernel_timespec ts = {
        .tv_sec = timeout_ms / 1000,
        .tv_nsec = (long long)(timeout_ms % 1000) * 1000000,
    };
    ret = io_uring_submit_and_wait_timeout(&reactor->ring, &cqe, 1, &ts, NULL);
  }
  if (ret < 0 && ret != -ETIME && ret != -EINTR) {
    return ret;
  }
  return dispatch(reactor);
}

static void uring_destroy(WioReactor *base) {
  UringReactor *reactor = uring_reactor(base);
  io_uring_queue_exit(&reactor->ring);
  free(reactor);
}

static const WioReactorOps uring_ops = {
    .destroy = uring_destroy,
    .submit = uring_submit,
    .cancel = uring_cancel,
    .cancel_fd = uring_cancel_fd,
    .wait = uring_wait,
};

int wio_uring_reactor_create(unsigned int depth, WioReactor **out) {
  if (depth > MAX_SQ_ENTRIES) {
    return -EINVAL;
  }

  UringReactor *reactor = calloc(1, sizeof(*reactor));
  if (reactor == NULL) {
    return -ENOMEM;
  }

  /* A multishot operation posts many completions, so the CQ ring is larger
   * than the default of twice the SQ ring. */
  struct io_uring_params params;
  memset(&params, 0, sizeof(params));
  params.flags =
      IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_CQSIZE;
  params.cq_entries = depth * CQ_ENTRIES_PER_SQ;
  if (params.cq_entries > MAX_CQ_ENTRIES) {
    params.cq_entries = MAX_CQ_ENTRIES;
  }
  int ret = io_uring_queue_init_params(depth, &reactor->ring, &params);
  if (ret < 0) {
    free(reactor);
    return ret;
  }

  reactor->base.ops = &uring_ops;
  reactor->base.depth = depth;
  *out = &reactor->base;
  return 0;
}
