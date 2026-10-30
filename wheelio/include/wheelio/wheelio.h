/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIO_H
#define WHEELIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * wheelio is a completion-style I/O reactor: the caller submits an operation,
 * the reactor executes it, and a callback fires when it has completed.
 *
 * Contract
 * --------
 *
 * Threading. A reactor belongs to one thread; none of its functions is safe to
 * call concurrently. Scale by creating one reactor per thread.
 *
 * Progress. Nothing happens until wio_wait() is called: submitting only
 * records the operation, and wio_wait() hands the recorded operations to the
 * backend, waits, and runs the callbacks of everything that has completed.
 * Callbacks run on the thread that called wio_wait(), may submit new
 * operations, and must not call wio_wait().
 *
 * Ownership. An operation is a WioOp owned by the caller, normally embedded
 * in the caller's own request state (see wio_container_of()). The reactor
 * allocates nothing per operation. The WioOp, and every buffer and structure
 * it was submitted with, must stay valid until the operation's final callback
 * has run, even if the operation is cancelled: the backend may keep reading
 * or writing them until then.
 *
 * Completion. Every operation completes with `result >= 0` (the
 * operation-specific value) or `result == -errno`. A single-shot operation
 * gets exactly one callback. A multishot operation gets a callback per event
 * with WIO_F_MORE set, and exactly one final callback without it; the
 * operation is finished, and its WioOp may be reused or freed, only after
 * that final callback.
 *
 * Errors. Functions that submit an operation return 0, or -errno if the
 * operation could not even be recorded (e.g. -EBUSY when `depth` operations
 * are already in flight). A returned error means no callback will follow.
 */

/**
 * I/O mechanism a reactor executes its operations with. Everything above the
 * reactor (protocol, server, client) is written once against the same
 * interface, so switching a backend is the only difference between two
 * benchmark runs.
 */
typedef enum {
  /* Nonblocking sockets + poll(); disk I/O is a blocking syscall made
   * inline by the loop itself. */
  WIO_BACKEND_LIBC = 0,
  /* Same as WIO_BACKEND_LIBC, but disk I/O runs on a pool of worker threads
   * (WIO_ALIBC_THREADS by default) and completes back into the loop. */
  WIO_BACKEND_ALIBC,
  /* io_uring. */
  WIO_BACKEND_URING,
} WioBackend;

/**
 * @return Name of the backend as it is spelled on a command line, or NULL
 *         if `backend` is not a valid value.
 */
const char *wio_backend_name(WioBackend backend);

/**
 * @param name Command-line spelling of a backend.
 * @param backend Receives the parsed value on success.
 *
 * @return 0 on success, -EINVAL if `name` is not a known backend.
 */
int wio_backend_from_name(const char *name, WioBackend *backend);

/** Size of the pool of disk worker threads of WIO_BACKEND_ALIBC, by default. */
#define WIO_ALIBC_THREADS 16u

typedef struct WioReactor WioReactor;
typedef struct WioOp WioOp;

/** Set in `flags` of a callback when more callbacks will follow. */
#define WIO_F_MORE (1u << 0)

/** Passed to wio_pwrite(): data must be durable when the write completes. */
#define WIO_WRITE_DSYNC (1u << 0)

/**
 * Completion callback of every operation except wio_recv_multishot().
 *
 * @param op     The operation that completed; use wio_container_of() to get
 *               to the caller's own state.
 * @param result Operation-specific value, or -errno:
 *               - wio_connect(), wio_close(), wio_fsync(): 0;
 *               - wio_accept(): the new descriptor;
 *               - wio_recv(), wio_send(), wio_sendmsg(), wio_pread(),
 *                 wio_pwrite(): the number of bytes transferred, which may
 *                 be less than requested (0 from a receive is end of
 *                 stream);
 *               A cancelled operation completes with -ECANCELED.
 * @param flags  WIO_F_MORE, or 0 for a final callback.
 */
typedef void (*WioCallback)(WioOp *op, int result, unsigned int flags);

/**
 * Completion callback of wio_recv_multishot().
 *
 * @param op     The operation that completed.
 * @param data   `result` bytes the reactor received, or NULL if `result <= 0`.
 *               Valid only until the callback returns: copy what is needed.
 * @param result Number of bytes in `data`; 0 is end of stream and -errno an
 *               error, both final.
 * @param flags  WIO_F_MORE, or 0 for a final callback.
 */
typedef void (*WioRecvCallback)(WioOp *op, const void *data, int result,
                                unsigned int flags);

/**
 * A single in-flight operation. The members are private to wheelio and the
 * layout may change; the structure is defined here only so that callers can
 * embed it by value.
 */
struct WioOp {
  union {
    WioCallback op;
    WioRecvCallback recv;
  } _cb;
  WioReactor *_reactor;
  bool _cancelled;
  void *_backend;
  /* State of the poll()-based backends: the request to run once its
   * descriptor is ready, and where the operation is in the reactor. */
  struct {
    WioOp *next;
    WioOp *prev;
    unsigned int list;
    unsigned int opcode;
    int fd;
    short events;
    unsigned int flags;
    int result;
    void *ptr;
    void *aux;
    size_t size;
    uint64_t offset;
  } _poll;
};

/**
 * Gets the structure `type` that embeds `ptr`, a pointer to its `member`.
 */
#define wio_container_of(ptr, type, member)                                    \
  ((type *)((char *)(ptr) - offsetof(type, member)))

/**
 * @param backend Mechanism the reactor executes operations with.
 * @param depth   Maximum number of operations in flight; must not be 0.
 * @param reactor   Receives the new reactor on success.
 *
 * @return 0 on success, -errno on failure (-EINVAL for a zero `depth`,
 *         -EPERM when io_uring is blocked by a seccomp profile).
 */
int wio_reactor_create(WioBackend backend, unsigned int depth,
                       WioReactor **reactor);

/**
 * Same as wio_reactor_create(), with the size of the pool of disk worker
 * threads, which only WIO_BACKEND_ALIBC uses.
 *
 * @param threads Worker threads of the pool; must not be 0.
 */
int wio_reactor_create_pool(WioBackend backend, unsigned int depth,
                            unsigned int threads, WioReactor **reactor);

/**
 * Destroys a reactor. No operation may be in flight: cancel them, and run
 * wio_wait() until wio_reactor_inflight() is 0.
 */
void wio_reactor_delete(WioReactor *reactor);

/**
 * @return Number of operations submitted and not yet finished, i.e. those
 *         whose final callback has not run.
 */
unsigned int wio_reactor_inflight(const WioReactor *reactor);

/**
 * Hands the submitted operations to the backend, waits for completions and
 * runs their callbacks.
 *
 * @param timeout_ms Longest time to wait in milliseconds: 0 to only collect
 *                   what has already completed, -1 to wait until something
 *                   completes.
 *
 * @return Number of callbacks run, 0 if the timeout expired first or nothing
 *         is in flight (nothing could ever complete, so it does not wait),
 *         or -errno on failure.
 */
int wio_wait(WioReactor *reactor, int timeout_ms);

/**
 * The libc backends make `fd` nonblocking.
 *
 * @param addr    Receives the peer address, may be NULL. Must stay valid
 *                until the callback.
 * @param addrlen In/out size of `addr`; NULL iff `addr` is NULL.
 */
int wio_accept(WioReactor *reactor, WioOp *op, int fd, struct sockaddr *addr,
               socklen_t *addrlen, WioCallback cb);

/**
 * The libc backends make `fd` nonblocking. `addr` must stay valid until the
 * callback.
 */
int wio_connect(WioReactor *reactor, WioOp *op, int fd,
                const struct sockaddr *addr, socklen_t addrlen, WioCallback cb);

/** Cancel everything in flight on `fd` first. */
int wio_close(WioReactor *reactor, WioOp *op, int fd, WioCallback cb);

/** `buf` must stay valid until the callback. */
int wio_recv(WioReactor *reactor, WioOp *op, int fd, void *buf, size_t size,
             unsigned int flags, WioCallback cb);

/**
 * Receives from `fd` until cancelled, end of stream or an error, with no
 * resubmission per chunk. The reactor owns the receive buffers: it takes
 * `entries` buffers of `entry_size` bytes, hands each received chunk to `cb`
 * and takes the buffer back when `cb` returns. A chunk is at most
 * `entry_size` bytes. When the buffers run out the reactor re-arms the receive
 * by itself and chunks keep coming.
 *
 * @param entries Number of buffers; a power of two, at most 32768.
 */
int wio_recv_multishot(WioReactor *reactor, WioOp *op, int fd,
                       size_t entry_size, unsigned int entries,
                       unsigned int flags, WioRecvCallback cb);

/**
 * MSG_NOSIGNAL is always added to `flags`. `buf` must stay valid until the
 * callback.
 */
int wio_send(WioReactor *reactor, WioOp *op, int fd, const void *buf,
             size_t size, unsigned int flags, WioCallback cb);

/**
 * Gathered send. MSG_NOSIGNAL is always added to `flags`. `msg` and
 * everything it points to must stay valid until the callback.
 */
int wio_sendmsg(WioReactor *reactor, WioOp *op, int fd,
                const struct msghdr *msg, unsigned int flags, WioCallback cb);

/**
 * Reads at `offset` without changing the file position. For a descriptor
 * opened with O_DIRECT, `buf`, `size` and `offset` must be aligned as
 * O_DIRECT requires. `buf` must stay valid until the callback.
 */
int wio_pread(WioReactor *reactor, WioOp *op, int fd, void *buf, size_t size,
              uint64_t offset, WioCallback cb);

/**
 * Writes at `offset` without changing the file position; see wio_pread() for
 * the alignment and lifetime of `buf`.
 *
 * @param flags WIO_WRITE_DSYNC to make the data durable before completing.
 */
int wio_pwrite(WioReactor *reactor, WioOp *op, int fd, const void *buf,
               size_t size, uint64_t offset, unsigned int flags,
               WioCallback cb);

/** @param datasync Skip metadata that is not needed to read the data back. */
int wio_fsync(WioReactor *reactor, WioOp *op, int fd, bool datasync,
              WioCallback cb);

/**
 * Asks for `target` to be cancelled. Its final callback still runs exactly
 * once, with -ECANCELED, unless the operation completed on its own first.
 * Fire and forget: cancelling has no callback of its own.
 *
 * `target` must not have had its final callback yet. If that cannot be
 * guaranteed, use wio_cancel_fd().
 *
 * @return 0 if the request was recorded, -errno otherwise.
 */
int wio_cancel(WioReactor *reactor, WioOp *target);

/**
 * Asks for every operation in flight on `fd` to be cancelled. Safe to call
 * when none is.
 */
int wio_cancel_fd(WioReactor *reactor, int fd);

#ifdef __cplusplus
}
#endif

#endif /* WHEELIO_H */
