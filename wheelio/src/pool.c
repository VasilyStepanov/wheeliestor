#include "pool.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

/* Operations waiting for a worker, and operations a worker has finished;
 * both are lists linked through WioOp::_poll.next. */
typedef struct {
  WioOp *head;
  WioOp *tail;
} Chain;

struct WioPool {
  WioPoolWork work;
  pthread_mutex_t mutex;
  pthread_cond_t wake_workers;
  Chain jobs;
  Chain done;
  bool stopping;
  /* A byte is written to wake[1] when `done` stops being empty. */
  int wake[2];
  unsigned int threads;
  pthread_t *workers;
};

static void chain_push(Chain *chain, WioOp *op) {
  op->_poll.next = NULL;
  if (chain->tail != NULL) {
    chain->tail->_poll.next = op;
  } else {
    chain->head = op;
  }
  chain->tail = op;
}

static WioOp *chain_pop(Chain *chain) {
  WioOp *op = chain->head;
  if (op != NULL) {
    chain->head = op->_poll.next;
    if (chain->head == NULL) {
      chain->tail = NULL;
    }
  }
  return op;
}

static void *worker_main(void *arg) {
  WioPool *pool = arg;

  pthread_mutex_lock(&pool->mutex);
  for (;;) {
    while (pool->jobs.head == NULL && !pool->stopping) {
      pthread_cond_wait(&pool->wake_workers, &pool->mutex);
    }
    WioOp *op = chain_pop(&pool->jobs);
    if (op == NULL) {
      break;
    }

    pthread_mutex_unlock(&pool->mutex);
    pool->work(op);
    pthread_mutex_lock(&pool->mutex);

    bool was_empty = pool->done.head == NULL;
    chain_push(&pool->done, op);
    if (was_empty) {
      /* The pipe is nonblocking. A full pipe means a wake-up is pending
       * already, so a failed write loses nothing. */
      char byte = 0;
      ssize_t ret = write(pool->wake[1], &byte, 1);
      (void)ret;
    }
  }
  pthread_mutex_unlock(&pool->mutex);
  return NULL;
}

static int make_wake_pipe(int fds[2]) {
  if (pipe(fds) < 0) {
    return -errno;
  }
  for (int i = 0; i < 2; ++i) {
    if (fcntl(fds[i], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) {
      int ret = -errno;
      close(fds[0]);
      close(fds[1]);
      return ret;
    }
  }
  return 0;
}

int wio_pool_create(unsigned int threads, WioPoolWork work, WioPool **out) {
  WioPool *pool = calloc(1, sizeof(*pool));
  if (pool == NULL) {
    return -ENOMEM;
  }
  pool->work = work;
  pool->workers = calloc(threads, sizeof(*pool->workers));
  if (pool->workers == NULL) {
    free(pool);
    return -ENOMEM;
  }

  int ret = make_wake_pipe(pool->wake);
  if (ret < 0) {
    free(pool->workers);
    free(pool);
    return ret;
  }
  pthread_mutex_init(&pool->mutex, NULL);
  pthread_cond_init(&pool->wake_workers, NULL);

  for (; pool->threads < threads; ++pool->threads) {
    ret =
        pthread_create(&pool->workers[pool->threads], NULL, worker_main, pool);
    if (ret != 0) {
      wio_pool_destroy(pool);
      return -ret;
    }
  }

  *out = pool;
  return 0;
}

void wio_pool_destroy(WioPool *pool) {
  pthread_mutex_lock(&pool->mutex);
  pool->stopping = true;
  pthread_cond_broadcast(&pool->wake_workers);
  pthread_mutex_unlock(&pool->mutex);

  for (unsigned int i = 0; i < pool->threads; ++i) {
    pthread_join(pool->workers[i], NULL);
  }

  pthread_cond_destroy(&pool->wake_workers);
  pthread_mutex_destroy(&pool->mutex);
  close(pool->wake[0]);
  close(pool->wake[1]);
  free(pool->workers);
  free(pool);
}

void wio_pool_submit(WioPool *pool, WioOp *op) {
  pthread_mutex_lock(&pool->mutex);
  chain_push(&pool->jobs, op);
  pthread_mutex_unlock(&pool->mutex);
  pthread_cond_signal(&pool->wake_workers);
}

WioOp *wio_pool_take_done(WioPool *pool) {
  /* Emptied before the list is taken: a worker that finishes in between has
   * its operation taken here, and its byte only makes the next poll return
   * early. */
  char bytes[64];
  while (read(pool->wake[0], bytes, sizeof(bytes)) > 0) {
  }

  pthread_mutex_lock(&pool->mutex);
  WioOp *done = pool->done.head;
  pool->done.head = NULL;
  pool->done.tail = NULL;
  pthread_mutex_unlock(&pool->mutex);
  return done;
}

int wio_pool_wake_fd(const WioPool *pool) { return pool->wake[0]; }
