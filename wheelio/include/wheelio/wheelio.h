/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIO_H
#define WHEELIO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * I/O mechanism a queue executes its operations with. Everything above the
 * queue (protocol, server, client) is written once against the same
 * completion-style interface, so switching a backend is the only difference
 * between two benchmark runs.
 */
typedef enum {
  /* Nonblocking sockets + poll(); disk I/O is a blocking syscall made
   * inline by the loop itself. */
  WIO_BACKEND_LIBC = 0,
  /* Same as WIO_BACKEND_LIBC, but disk I/O runs on a pool of worker
   * threads and completes back into the loop. */
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

#ifdef __cplusplus
}
#endif

#endif /* WHEELIO_H */
