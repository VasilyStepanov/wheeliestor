/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIE_CLIENT_H
#define WHEELIE_CLIENT_H

#include <wheelie/protocol.h>

#include <wheelio/wheelio.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A client of a wheelie server: one connection and the requests in flight on
 * it. It belongs to one thread, and nothing in it is safe to call from
 * another.
 *
 * Requests are submitted without waiting and finish later, through their
 * callbacks, which run inside wheelie_client_wait(), the only thing that
 * makes progress. A callback may submit requests, but must not call
 * wheelie_client_wait() or delete the client.
 */
typedef struct WheelieClient WheelieClient;

typedef struct {
  /* The server: a name or an IPv4 address, and a port. */
  const char *host;
  uint16_t port;
  /* The I/O mechanism the client uses on its connection. */
  WioBackend backend;
  /* Receive with one multishot receive instead of a receive per chunk. */
  bool multishot;
  /* The most requests in flight at a time. */
  unsigned int depth;
} WheelieClientOptions;

/**
 * Called once for every request that has finished.
 *
 * @param res  The number of bytes read or written, or -errno. A request that
 *             was in flight when the connection broke finishes with the error
 *             that broke it; one in flight when the client is deleted, with
 *             -ECANCELED.
 * @param data The pointer given with the request.
 */
typedef void (*WheelieCallback)(int64_t res, void *data);

/**
 * Connects to the server, which it waits for.
 *
 * @return 0 on success, -errno on failure.
 */
int wheelie_client_create(const WheelieClientOptions *options,
                          WheelieClient **client);

/**
 * Disconnects. The requests still in flight finish through their callbacks,
 * with -ECANCELED, before it returns.
 */
void wheelie_client_delete(WheelieClient *client);

/**
 * Submits a read of `size` bytes at `offset` into `buf`, which has to stay
 * valid until the callback.
 *
 * @return 0 on success, or -errno: -EBUSY if `depth` requests are in flight,
 *         -EMSGSIZE for a size larger than WHEELIE_MAX_PAYLOAD_SIZE, or the
 *         error that broke the connection.
 */
int wheelie_client_pread(WheelieClient *client, void *buf, size_t size,
                         uint64_t offset, WheelieCallback callback, void *data);

/**
 * Submits a write of `size` bytes at `offset` from `buf`, which has to stay
 * valid until the callback.
 *
 * @param flags WHEELIE_FLAG_SYNC to have the data durable when the request
 *              finishes, or 0.
 *
 * @return As wheelie_client_pread(), and -EINVAL for a flag that is unknown.
 */
int wheelie_client_pwrite(WheelieClient *client, const void *buf, size_t size,
                          uint64_t offset, unsigned int flags,
                          WheelieCallback callback, void *data);

/**
 * Runs what has completed on the connection, and so the callbacks of the
 * requests that have finished, waiting for at least one completion.
 *
 * @param timeout_ms Longest time to wait in milliseconds: 0 to only collect
 *                   what has completed already, -1 to wait until something
 *                   does. The connection always has an operation in flight,
 *                   so -1 waits for good if no request will ever finish.
 *
 * @return 0, or -errno on failure.
 */
int wheelie_client_wait(WheelieClient *client, int timeout_ms);

/** @return Number of requests submitted and not finished yet. */
unsigned int wheelie_client_inflight(const WheelieClient *client);

#ifdef __cplusplus
}
#endif

#endif /* WHEELIE_CLIENT_H */
