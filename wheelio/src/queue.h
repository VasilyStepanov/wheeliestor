#ifndef WHEELIO_SRC_QUEUE_H
#define WHEELIO_SRC_QUEUE_H

#include <wheelio/wheelio.h>

/* What an operation asks for; stored in WioOp::_kind. */
enum {
  WIO_KIND_ACCEPT = 1,
  WIO_KIND_CONNECT,
  WIO_KIND_CLOSE,
  WIO_KIND_RECV,
  WIO_KIND_RECV_MULTISHOT,
  WIO_KIND_SEND,
  WIO_KIND_SENDMSG,
  WIO_KIND_PREAD,
  WIO_KIND_PWRITE,
  WIO_KIND_FSYNC,
};

#define WIO_RECV_MAX_ENTRIES 32768u

typedef struct WioQueueOps {
  void (*destroy)(WioQueue *queue);
  /* Records `op`, whose request is already in its private members. */
  int (*submit)(WioQueue *queue, WioOp *op);
  int (*cancel)(WioQueue *queue, WioOp *target);
  int (*cancel_fd)(WioQueue *queue, int fd);
  int (*wait)(WioQueue *queue, int timeout_ms);
} WioQueueOps;

/* Common head of every backend's queue. */
struct WioQueue {
  const WioQueueOps *ops;
  unsigned int depth;
  unsigned int inflight;
};

/*
 * Backends report completions through these. A completion without WIO_F_MORE
 * is the final one: the operation stops counting as in flight and its
 * memory belongs to the caller again before the callback runs.
 */
void wio_queue_complete(WioOp *op, int result, unsigned int flags);
void wio_queue_complete_recv(WioOp *op, const void *data, int result,
                             unsigned int flags);

int wio_uring_queue_create(unsigned int depth, WioQueue **queue);

#endif /* WHEELIO_SRC_QUEUE_H */
