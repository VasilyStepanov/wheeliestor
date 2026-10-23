/* The public header has to be C: it is what an engine written in C includes.
 * This file only has to compile. */
#include <wheelie/client.h>

static void on_done(int64_t res, void *data) {
  (void)res;
  (void)data;
}

int wheelie_header_is_c(void) {
  WheelieClientOptions options = {"localhost", 1, WIO_BACKEND_LIBC,
                                  false,       4, 1};
  WheelieClient *client = NULL;
  char buf[16];
  int ret = wheelie_client_create(&options, &client);
  if (ret < 0) {
    return ret;
  }
  ret = wheelie_client_pread(client, buf, sizeof(buf), 0, on_done, NULL);
  ret += wheelie_client_pwrite(client, buf, sizeof(buf), 0, WHEELIE_FLAG_SYNC,
                               on_done, NULL);
  ret += wheelie_client_wait(client, 0);
  ret += (int)wheelie_client_inflight(client);
  wheelie_client_delete(client);
  return ret;
}
