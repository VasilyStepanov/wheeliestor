#include <wheelio/wheelio.h>

#include <errno.h>
#include <string.h>

static const char *const backend_names[] = {
    [WIO_BACKEND_LIBC] = "libc",
    [WIO_BACKEND_ALIBC] = "alibc",
    [WIO_BACKEND_URING] = "uring",
};

#define BACKENDS_COUNT (sizeof(backend_names) / sizeof(backend_names[0]))

const char *wio_backend_name(WioBackend backend) {
  if ((unsigned int)backend >= BACKENDS_COUNT) {
    return NULL;
  }
  return backend_names[backend];
}

int wio_backend_from_name(const char *name, WioBackend *backend) {
  if (name == NULL) {
    return -EINVAL;
  }
  for (unsigned int i = 0; i < BACKENDS_COUNT; ++i) {
    if (strcmp(name, backend_names[i]) == 0) {
      *backend = (WioBackend)i;
      return 0;
    }
  }
  return -EINVAL;
}
