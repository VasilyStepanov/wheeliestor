#include <client/client.hpp>

#include <wheelie/client.h>

#include <wheelio/coro.hpp>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <system_error>

struct WheelieClient {
  wheelie::Client client;

  explicit WheelieClient(const wheelie::ClientOptions &options)
      : client(options) {}
};

namespace {

// Runs a request for the C interface: it is submitted at once, and what
// becomes of it is reported through `callback`, from this coroutine. A request
// that cannot be submitted is reported through `submitted` instead, and has no
// callback. `submitted` is on the stack of the caller, so it is only touched
// before the first suspension.
template <typename Submit>
wheelio::DetachedTask run(Submit submit, WheelieCallback callback, void *data,
                          int &submitted) {
  int64_t res;
  bool queued = false;
  try {
    auto request = submit();
    queued = true;
    submitted = 0;
    res = static_cast<int64_t>(co_await request);
  } catch (const std::system_error &e) {
    if (!queued) {
      submitted = -e.code().value();
      co_return;
    }
    res = -e.code().value();
  } catch (...) {
    if (!queued) {
      submitted = -EIO;
      co_return;
    }
    res = -EIO;
  }
  callback(res, data);
}

} // namespace

extern "C" {

int wheelie_location_parse(const char *location, char *host, size_t host_size,
                           uint16_t *port) {
  static const char scheme[] = "wheelie://";

  if (location == nullptr || host == nullptr || port == nullptr) {
    return -EINVAL;
  }
  if (std::strncmp(location, scheme, sizeof(scheme) - 1) != 0) {
    return -EINVAL;
  }
  const char *endpoint = location + sizeof(scheme) - 1;
  const char *colon = std::strrchr(endpoint, ':');
  if (colon == nullptr || colon == endpoint) {
    return -EINVAL;
  }

  size_t host_length = static_cast<size_t>(colon - endpoint);
  if (host_length >= host_size || std::memchr(endpoint, '/', host_length)) {
    return -EINVAL;
  }

  // Only digits: strtol() would take a sign and a space as well.
  const char *digits = colon + 1;
  if (*digits == '\0') {
    return -EINVAL;
  }
  for (const char *c = digits; *c != '\0'; ++c) {
    if (!std::isdigit(static_cast<unsigned char>(*c))) {
      return -EINVAL;
    }
  }
  unsigned long value = std::strtoul(digits, nullptr, 10);
  if (value < 1 || value > 65535) {
    return -EINVAL;
  }

  std::memcpy(host, endpoint, host_length);
  host[host_length] = '\0';
  *port = static_cast<uint16_t>(value);
  return 0;
}

int wheelie_client_create(const WheelieClientOptions *options,
                          WheelieClient **client) {
  if (options == nullptr || client == nullptr || options->host == nullptr) {
    return -EINVAL;
  }
  wheelie::ClientOptions cpp_options;
  cpp_options.host = options->host;
  cpp_options.port = options->port;
  cpp_options.backend = options->backend;
  cpp_options.multishot = options->multishot;
  cpp_options.depth = options->depth;
  try {
    *client = new WheelieClient(cpp_options);
    return 0;
  } catch (const std::system_error &e) {
    return -e.code().value();
  } catch (const std::bad_alloc &) {
    return -ENOMEM;
  } catch (...) {
    return -EINVAL;
  }
}

void wheelie_client_delete(WheelieClient *client) { delete client; }

int wheelie_client_pread(WheelieClient *client, void *buf, size_t size,
                         uint64_t offset, WheelieCallback callback,
                         void *data) {
  int submitted = -EIO;
  run([&] { return client->client.pread(buf, size, offset); }, callback, data,
      submitted);
  return submitted;
}

int wheelie_client_pwrite(WheelieClient *client, const void *buf, size_t size,
                          uint64_t offset, unsigned int flags,
                          WheelieCallback callback, void *data) {
  int submitted = -EIO;
  run([&] { return client->client.pwrite(buf, size, offset, flags); }, callback,
      data, submitted);
  return submitted;
}

int wheelie_client_wait(WheelieClient *client, int timeout_ms) {
  try {
    client->client.reactor().wait(timeout_ms);
    return 0;
  } catch (const std::system_error &e) {
    return -e.code().value();
  } catch (...) {
    return -EINVAL;
  }
}

unsigned int wheelie_client_inflight(const WheelieClient *client) {
  return client->client.inflight();
}

} // extern "C"
