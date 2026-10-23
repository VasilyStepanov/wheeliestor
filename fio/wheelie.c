/*
 * wheelie engine
 *
 * An io engine of fio for a wheelie server (see docs/protocol.md), loaded as
 * an external engine:
 *
 *   fio --ioengine=external:/path/to/wheelie.so \
 *       --filename=wheelie\\://127.0.0.1\\:39900 --size=1g ...
 *
 * The file name of the job is the location of the server, wheelie://HOST:PORT.
 * fio splits file names at colons, so the colons of a location are escaped
 * with a backslash; fio takes the backslashes out before the engine sees it.
 *
 * The protocol has no way to ask the server for the size of what it serves,
 * so a job has to pass --size.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wheelie/client.h>

#include <fio.h>
#include <optgroup.h>

struct wheelie_options {
  /* An option must not be at offset 0 of the structure. */
  void *pad;
  char *backend;
  unsigned int multishot;
  unsigned int queues;
};

static struct fio_option options[] = {
    {
        .name = "wheelie_backend",
        .lname = "wheelie backend",
        .type = FIO_OPT_STR_STORE,
        .off1 = offsetof(struct wheelie_options, backend),
        .help = "I/O mechanism of the client: libc, alibc or uring",
        .def = "uring",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "wheelie_multishot",
        .lname = "wheelie multishot receive",
        .type = FIO_OPT_BOOL,
        .off1 = offsetof(struct wheelie_options, multishot),
        .help = "Receive the responses with one multishot receive instead of "
                "a receive per chunk",
        .def = "0",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = "wheelie_queues",
        .lname = "wheelie queues",
        .type = FIO_OPT_INT,
        .off1 = offsetof(struct wheelie_options, queues),
        .minval = 1,
        .maxval = 64,
        .help = "Connections to the server, over which the requests are "
                "spread",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_INVALID,
    },
    {
        .name = NULL,
    },
};

struct wheelie_data {
  WheelieClient *client;
  /* Flags of a write: WHEELIE_FLAG_SYNC if the job asks for sync writes. */
  unsigned int write_flags;
  /* The requests that have finished and that fio has not been given yet;
   * the callbacks put them here. */
  struct io_u **events;
  unsigned int nr_events;
  /* How many of them the last getevents() handed to fio. */
  unsigned int nr_returned;
};

static void io_callback(int64_t res, void *data) {
  struct io_u *io_u = data;
  struct wheelie_data *wd = io_u->engine_data;

  if (res < 0) {
    io_u->error = (int)-res;
    io_u->resid = io_u->xfer_buflen;
  } else {
    io_u->error = 0;
    io_u->resid = io_u->xfer_buflen - (unsigned long long)res;
  }
  wd->events[wd->nr_events++] = io_u;
}

static enum fio_q_status fio_wheelie_queue(struct thread_data *td,
                                           struct io_u *io_u) {
  struct wheelie_data *wd = td->io_ops_data;
  int ret;

  fio_ro_check(td, io_u);

  if (io_u->ddir == DDIR_READ) {
    ret = wheelie_client_pread(wd->client, io_u->xfer_buf, io_u->xfer_buflen,
                               io_u->offset, io_callback, io_u);
  } else if (io_u->ddir == DDIR_WRITE) {
    ret =
        wheelie_client_pwrite(wd->client, io_u->xfer_buf, io_u->xfer_buflen,
                              io_u->offset, wd->write_flags, io_callback, io_u);
  } else {
    /* The protocol has no trim and no sync operation. */
    if (wheelie_client_inflight(wd->client)) {
      return FIO_Q_BUSY;
    }
    return FIO_Q_COMPLETED;
  }

  if (ret == -EBUSY) {
    return FIO_Q_BUSY;
  }
  if (ret < 0) {
    io_u->error = -ret;
    td_verror(td, io_u->error, "xfer");
    log_err("wheelie: failed to submit a request: %s\n", strerror(-ret));
    return FIO_Q_COMPLETED;
  }
  return FIO_Q_QUEUED;
}

static long long milliseconds_since(const struct timespec *start) {
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long long)(now.tv_sec - start->tv_sec) * 1000 +
         (now.tv_nsec - start->tv_nsec) / 1000000;
}

static int fio_wheelie_getevents(struct thread_data *td, unsigned int min,
                                 unsigned int max, const struct timespec *t) {
  struct wheelie_data *wd = td->io_ops_data;
  struct timespec start;
  long long limit_ms = -1;
  int ret;

  /* What fio was given last time is gone from the list. */
  if (wd->nr_returned) {
    memmove(wd->events, wd->events + wd->nr_returned,
            (wd->nr_events - wd->nr_returned) * sizeof(*wd->events));
    wd->nr_events -= wd->nr_returned;
    wd->nr_returned = 0;
  }

  /*
   * Ready events can satisfy the caller without entering the reactor.
   * Requests queued meanwhile are submitted when these events run out;
   * a non-blocking poll still has to submit them even if it needs no events.
   */
  if (min > 0 && wd->nr_events >= min) {
    wd->nr_returned = wd->nr_events < max ? wd->nr_events : max;
    return wd->nr_returned;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);
  if (t) {
    limit_ms = (long long)t->tv_sec * 1000 + t->tv_nsec / 1000000;
  }

  /*
   * Every wait also submits what has been queued since the last one, so the
   * first wait is blocking only if fio needs more events than it has and
   * something is in flight to supply them: a non-blocking wait ahead of a
   * blocking one costs a second system call when nothing has completed yet.
   */
  for (;;) {
    long long left_ms = 0;

    if (wd->nr_events < min && wheelie_client_inflight(wd->client) > 0) {
      left_ms = -1;
      if (t) {
        left_ms = limit_ms - milliseconds_since(&start);
        if (left_ms < 0) {
          left_ms = 0;
        }
      }
    }
    ret = wheelie_client_wait(wd->client, (int)left_ms);

    if (ret != 0 || wd->nr_events >= min ||
        wheelie_client_inflight(wd->client) == 0 || left_ms == 0) {
      break;
    }
  }
  if (ret < 0) {
    log_err("wheelie: wait failed: %s\n", strerror(-ret));
    td_verror(td, -ret, "xfer");
  }

  wd->nr_returned = wd->nr_events < max ? wd->nr_events : max;
  return wd->nr_returned;
}

static struct io_u *fio_wheelie_event(struct thread_data *td, int event) {
  struct wheelie_data *wd = td->io_ops_data;

  return wd->events[event];
}

static int fio_wheelie_io_u_init(struct thread_data *td, struct io_u *io_u) {
  io_u->engine_data = td->io_ops_data;
  return 0;
}

static void fio_wheelie_io_u_free(struct thread_data *td, struct io_u *io_u) {
  (void)td;
  io_u->engine_data = NULL;
}

static int fio_wheelie_open_file(struct thread_data *td, struct fio_file *f) {
  struct wheelie_data *wd = td->io_ops_data;
  struct wheelie_options *o = td->eo;
  WheelieClientOptions client_options;
  char host[256];
  uint16_t port;
  int ret;

  if (wheelie_location_parse(f->file_name, host, sizeof(host), &port) != 0) {
    log_err("wheelie: the file name must be wheelie://HOST:PORT, with the "
            "colons escaped by backslashes, not '%s'\n",
            f->file_name);
    return 1;
  }

  memset(&client_options, 0, sizeof(client_options));
  client_options.host = host;
  client_options.port = port;
  client_options.multishot = o->multishot != 0;
  client_options.depth = td->o.iodepth;
  client_options.queues = o->queues;
  if (wio_backend_from_name(o->backend ? o->backend : "uring",
                            &client_options.backend) != 0) {
    log_err("wheelie: wheelie_backend must be libc, alibc or uring\n");
    return 1;
  }

  /*
   * fio's own --sync (none against sync or dsync) is the question the flag of
   * a write answers: is it durable when the request finishes? The server
   * gives both the same, a write with RWF_DSYNC.
   */
  wd->write_flags = td->o.sync_io != 0 ? WHEELIE_FLAG_SYNC : 0;

  ret = wheelie_client_create(&client_options, &wd->client);
  if (ret != 0) {
    log_err("wheelie: connecting to %s:%u failed: %s\n", host, port,
            strerror(-ret));
    td_verror(td, -ret, "wheelie_client_create");
    return 1;
  }
  return 0;
}

static int fio_wheelie_close_file(struct thread_data *td, struct fio_file *f) {
  struct wheelie_data *wd = td->io_ops_data;

  (void)f;

  if (wd->client) {
    wheelie_client_delete(wd->client);
    wd->client = NULL;
  }
  return 0;
}

static int fio_wheelie_init(struct thread_data *td) {
  struct wheelie_data *wd = calloc(1, sizeof(*wd));

  if (!wd) {
    return 1;
  }
  wd->events = calloc(td->o.iodepth, sizeof(*wd->events));
  if (!wd->events) {
    free(wd);
    return 1;
  }

  td->io_ops_data = wd;
  return 0;
}

static void fio_wheelie_cleanup(struct thread_data *td) {
  struct wheelie_data *wd = td->io_ops_data;

  if (wd) {
    free(wd->events);
    free(wd);
  }
}

/* An external engine is found by this name, and registers itself no other
 * way. */
struct ioengine_ops ioengine = {
    .name = "wheelie",
    .version = FIO_IOOPS_VERSION,
    .init = fio_wheelie_init,
    .cleanup = fio_wheelie_cleanup,
    .open_file = fio_wheelie_open_file,
    .close_file = fio_wheelie_close_file,
    .queue = fio_wheelie_queue,
    .getevents = fio_wheelie_getevents,
    .event = fio_wheelie_event,
    .io_u_init = fio_wheelie_io_u_init,
    .io_u_free = fio_wheelie_io_u_free,
    .options = options,
    .option_struct_size = sizeof(struct wheelie_options),
    /* Not FIO_ASYNCIO_SETS_ISSUE_TIME: queue() does not stamp the time of
     * issue, so fio does it right after queue() returns. */
    .flags = FIO_DISKLESSIO | FIO_NOEXTEND,
};
