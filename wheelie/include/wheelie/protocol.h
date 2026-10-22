/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIE_PROTOCOL_H
#define WHEELIE_PROTOCOL_H

#include <stdint.h>

/*
 * The wheelie wire protocol: block access over TCP, deliberately bare. There
 * is no magic number, no version and no authentication, and integers are in
 * the host byte order: it connects a client and a server built together on
 * machines of one kind, to compare I/O mechanisms.
 *
 * A request is a WheelieRequestHeader, followed by `size` bytes of payload for
 * WHEELIE_OP_WRITE (a read has no payload). A
 * response is a WheelieResponseHeader, followed by `res` bytes of payload when
 * it answers a WHEELIE_OP_READ and `res` is positive.
 *
 * `cid` is chosen by the client and only echoed back: it is how a client with
 * several requests in flight on one connection tells which one a response
 * belongs to, since responses may come in any order.
 *
 * A write answers with the number of bytes written, or -errno. What has
 * happened to the data when the response is sent depends on the flags: with
 * WHEELIE_FLAG_SYNC it is durable, without it is not necessarily.
 *
 * `op` says what to do, and `flags` how. A flag that is unknown, or does not
 * apply to the operation, ends the connection like an unknown operation.
 *
 * A server that opened its file with O_DIRECT passes `offset` and `size` on as
 * they are, so they have to be aligned as O_DIRECT requires, or the request
 * fails with -EINVAL.
 */

#ifdef __cplusplus
extern "C" {
#endif

enum {
  WHEELIE_OP_READ = 0,
  WHEELIE_OP_WRITE = 1,
};

/* Flags of a request, in WheelieRequestHeader::flags. */

/* A write: the data is durable when the response is sent. */
#define WHEELIE_FLAG_SYNC (1u << 0)

/* The most one request may carry or ask for. A larger read is answered with
 * -EMSGSIZE; a larger write ends the connection, since its payload is
 * already on the way. */
#define WHEELIE_MAX_PAYLOAD_SIZE ((uint64_t)4 * 1024 * 1024)

#define WHEELIE_PACKED __attribute__((packed))

struct WheelieRequestHeader {
  uint64_t cid;
  uint64_t offset;
  uint64_t size;
  uint8_t op;
  uint8_t flags;
} WHEELIE_PACKED;

struct WheelieResponseHeader {
  uint64_t cid;
  /* Bytes transferred, or -errno. */
  int64_t res;
} WHEELIE_PACKED;

#ifdef __cplusplus
static_assert(sizeof(struct WheelieRequestHeader) == 26,
              "WheelieRequestHeader is 26 bytes on the wire");
static_assert(sizeof(struct WheelieResponseHeader) == 16,
              "WheelieResponseHeader is 16 bytes on the wire");
#else
_Static_assert(sizeof(struct WheelieRequestHeader) == 26,
               "WheelieRequestHeader is 26 bytes on the wire");
_Static_assert(sizeof(struct WheelieResponseHeader) == 16,
               "WheelieResponseHeader is 16 bytes on the wire");
#endif

#ifdef __cplusplus
}
#endif

#endif /* WHEELIE_PROTOCOL_H */
