#ifndef WHEELIO_SRC_POOL_H
#define WHEELIO_SRC_POOL_H

#include <wheelio/wheelio.h>

/*
 * A pool of worker threads that run blocking work for a reactor. The
 * reactor hands an operation over with wio_pool_submit(); one of the workers
 * runs `work` on it, and the finished operation waits for the reactor to
 * take it with wio_pool_take_done(). The reactor learns that something has
 * finished from wio_pool_wake_fd(), which it polls for readability.
 *
 * While an operation is in the pool the reactor must not touch it, and the
 * pool links it through WioOp::_poll.next.
 */
typedef struct WioPool WioPool;

typedef void (*WioPoolWork)(WioOp *op);

int wio_pool_create(unsigned int threads, WioPoolWork work, WioPool **pool);

/* Stops the workers once they have run what they were given. */
void wio_pool_destroy(WioPool *pool);

void wio_pool_submit(WioPool *pool, WioOp *op);

/* @return Every operation finished since the last call, linked through
 *         WioOp::_poll.next, or NULL if there is none. */
WioOp *wio_pool_take_done(WioPool *pool);

int wio_pool_wake_fd(const WioPool *pool);

#endif /* WHEELIO_SRC_POOL_H */
