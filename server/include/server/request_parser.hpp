#ifndef WHEELIE_SERVER_REQUEST_PARSER_HPP
#define WHEELIE_SERVER_REQUEST_PARSER_HPP

#include "aligned_buffer.hpp"

#include <wheelie/protocol.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

namespace wheelie {

// A request that breaks the protocol; nothing that follows it on the
// connection can be trusted.
class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct Request {
  WheelieRequestHeader header;
  // The payload of a write, in an aligned buffer; empty for a read.
  AlignedBuffer payload;
};

/**
 * Cuts a stream of bytes into requests. The bytes come in pieces of any size,
 * which have nothing to do with the borders of the requests: a piece may hold
 * several requests, or a part of one. The payload of a write is copied
 * straight into its aligned buffer.
 */
class RequestParser final {
public:
  // Consumes `bytes` and calls `on_request(Request &&)` for each request that
  // is complete. Throws ProtocolError for an unknown operation or a write
  // larger than WHEELIE_MAX_PAYLOAD_SIZE.
  template <typename OnRequest>
  void feed(std::span<const std::byte> bytes, OnRequest &&on_request) {
    while (!bytes.empty()) {
      if (!_in_payload) {
        size_t take = std::min(sizeof(_header_bytes) - _have, bytes.size());
        std::memcpy(_header_bytes + _have, bytes.data(), take);
        _have += take;
        bytes = bytes.subspan(take);
        if (_have < sizeof(_header_bytes)) {
          return;
        }

        _have = 0;
        std::memcpy(&_request.header, _header_bytes, sizeof(_header_bytes));
        if (_request.header.op != WHEELIE_OP_READ &&
            _request.header.op != WHEELIE_OP_WRITE &&
            _request.header.op != WHEELIE_OP_WRITE_SYNC) {
          throw ProtocolError("wheelie: unknown request operation");
        }
        if (_request.header.op != WHEELIE_OP_READ) {
          if (_request.header.size > WHEELIE_MAX_PAYLOAD_SIZE) {
            throw ProtocolError("wheelie: write larger than the maximum");
          }
          if (_request.header.size > 0) {
            _request.payload = AlignedBuffer(_request.header.size);
            _filled = 0;
            _in_payload = true;
            continue;
          }
        }
        finish(on_request);
      } else {
        size_t take = std::min(_request.payload.size() - _filled, bytes.size());
        std::memcpy(_request.payload.data() + _filled, bytes.data(), take);
        _filled += take;
        bytes = bytes.subspan(take);
        if (_filled == _request.payload.size()) {
          finish(on_request);
        }
      }
    }
  }

  // True while a request has begun and is not complete: the stream must not
  // end here.
  bool mid_request() const noexcept { return _have > 0 || _in_payload; }

private:
  template <typename OnRequest> void finish(OnRequest &on_request) {
    _in_payload = false;
    Request done = std::exchange(_request, Request{});
    on_request(std::move(done));
  }

  std::byte _header_bytes[sizeof(WheelieRequestHeader)];
  size_t _have = 0;
  bool _in_payload = false;
  size_t _filled = 0;
  Request _request = {};
};

} // namespace wheelie

#endif // WHEELIE_SERVER_REQUEST_PARSER_HPP
