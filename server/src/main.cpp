#include <server/server.hpp>

#include <wheelio/wheelio.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include <getopt.h>
#include <pthread.h>
#include <signal.h>

namespace {

void print_usage(const char *argv0) {
  std::fprintf(
      stderr,
      "Usage: %s -b HOST:PORT -f FILE [options]\n"
      "\n"
      "Options:\n"
      "  -b, --bind HOST:PORT     address to listen on (required)\n"
      "  -f, --file PATH          file or block device to serve (required)\n"
      "  -B, --backend NAME       libc, alibc or uring (default: uring)\n"
      "      --multishot          receive with one multishot receive per\n"
      "                           connection (default: a receive per chunk)\n"
      "  -t, --threads N          threads, each with a reactor of its own\n"
      "                           (default: number of CPUs)\n"
      "      --queue-depth N      operations a reactor may have in flight\n"
      "                           (default: 1024)\n"
      "      --direct 0|1         open the file with O_DIRECT (default: 1)\n"
      "  -h, --help               show this help and exit\n",
      argv0);
}

unsigned int parse_number(const std::string &text, const char *what) {
  char *end = nullptr;
  unsigned long value = std::strtoul(text.c_str(), &end, 10);
  if (text.empty() || *end != '\0' || value == 0 || value > 1u << 20) {
    throw std::invalid_argument(std::string("bad ") + what);
  }
  return static_cast<unsigned int>(value);
}

void parse_bind(const std::string &text, wheelie::ServerConfig &config) {
  size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == text.size()) {
    throw std::invalid_argument("--bind must be HOST:PORT");
  }
  config.host = text.substr(0, colon);
  unsigned int port = parse_number(text.substr(colon + 1), "--bind port");
  if (port > 65535) {
    throw std::invalid_argument("--bind port out of range");
  }
  config.port = static_cast<uint16_t>(port);
}

} // namespace

int main(int argc, char **argv) {
  wheelie::ServerConfig config;
  config.threads = std::max(1u, std::thread::hardware_concurrency());
  bool have_bind = false;

  static const option options[] = {
      {"bind", required_argument, nullptr, 'b'},
      {"file", required_argument, nullptr, 'f'},
      {"backend", required_argument, nullptr, 'B'},
      {"multishot", no_argument, nullptr, 'M'},
      {"threads", required_argument, nullptr, 't'},
      {"queue-depth", required_argument, nullptr, 'Q'},
      {"direct", required_argument, nullptr, 'D'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  try {
    int option;
    while ((option = getopt_long(argc, argv, "b:f:B:t:h", options, nullptr)) !=
           -1) {
      switch (option) {
      case 'b':
        parse_bind(optarg, config);
        have_bind = true;
        break;
      case 'f':
        config.file = optarg;
        break;
      case 'B':
        if (wio_backend_from_name(optarg, &config.backend) != 0) {
          throw std::invalid_argument("--backend must be libc, alibc or uring");
        }
        break;
      case 'M':
        config.multishot = true;
        break;
      case 't':
        config.threads = parse_number(optarg, "--threads");
        break;
      case 'Q':
        config.queue_depth = parse_number(optarg, "--queue-depth");
        break;
      case 'D':
        if (std::string(optarg) != "0" && std::string(optarg) != "1") {
          throw std::invalid_argument("--direct must be 0 or 1");
        }
        config.direct = std::string(optarg) == "1";
        break;
      case 'h':
        print_usage(argv[0]);
        return 0;
      default:
        print_usage(argv[0]);
        return 2;
      }
    }
    if (!have_bind || config.file.empty()) {
      print_usage(argv[0]);
      return 2;
    }

    // The signals are taken by sigwait() below, not by a handler: blocked
    // here, they stay blocked in the threads the server starts.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    wheelie::Server server(config);
    uint16_t port = server.start();
    std::fprintf(stderr, "wheelie-server: listening on %s:%u (%s%s, %u)\n",
                 config.host.c_str(), port, wio_backend_name(config.backend),
                 config.multishot ? " multishot" : "", config.threads);

    int signal_number = 0;
    sigwait(&signals, &signal_number);
    std::fprintf(stderr, "wheelie-server: shutting down\n");
    server.stop();
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "wheelie-server: %s\n", e.what());
    return 1;
  }
}
