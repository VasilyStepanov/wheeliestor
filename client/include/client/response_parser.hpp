#ifndef WHEELIE_CLIENT_RESPONSE_PARSER_HPP
#define WHEELIE_CLIENT_RESPONSE_PARSER_HPP

#include <wheelie/protocol.h>
#include <wheelie/protocol_error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace wheelie {

// Where the data of a read goes: the buffer the caller gave, and its size. A
// null buffer says that the request is a write, whose response has no data.
struct ResponseTarget {
  std::byte *buffer;
  size_t size;
};

/**
 * Cuts a stream of bytes into responses. The bytes come in pieces of any size,
 * which have nothing to do with the borders of the responses. The data of a
 * read is copied straight into the buffer of the request it answers.
 */
class ResponseParser final {
public:
  // Consumes `bytes`. `lookup(cid)` says what request `cid` is, as a
  // std::optional<ResponseTarget>, empty if it is not in flight; the request
  // has to stay in flight until the response is complete.
  // `on_response(cid, res)` is called for each response that is complete.
  // Throws ProtocolError for a response to a request that is not in flight, or
  // that carries more data than was asked for.
  template <typename Lookup, typename OnResponse>
  void feed(std::span<const std::byte> bytes, Lookup &&lookup,
            OnResponse &&on_response) {
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
        std::memcpy(&_header, _header_bytes, sizeof(_header_bytes));
        std::optional<ResponseTarget> target = lookup(_header.cid);
        if (!target) {
          throw ProtocolError(
              "wheelie: response to a request that is not in flight");
        }
        if (target->buffer != nullptr && _header.res > 0) {
          if (static_cast<uint64_t>(_header.res) > target->size) {
            throw ProtocolError("wheelie: response larger than the request");
          }
          _target = target->buffer;
          _expected = static_cast<size_t>(_header.res);
          _filled = 0;
          _in_payload = true;
          continue;
        }
        on_response(_header.cid, _header.res);
      } else {
        size_t take = std::min(_expected - _filled, bytes.size());
        std::memcpy(_target + _filled, bytes.data(), take);
        _filled += take;
        bytes = bytes.subspan(take);
        if (_filled == _expected) {
          _in_payload = false;
          on_response(_header.cid, _header.res);
        }
      }
    }
  }

  // True while a response has begun and is not complete: the stream must not
  // end here.
  bool mid_response() const noexcept { return _have > 0 || _in_payload; }

private:
  std::byte _header_bytes[sizeof(WheelieResponseHeader)];
  size_t _have = 0;
  WheelieResponseHeader _header = {};
  bool _in_payload = false;
  std::byte *_target = nullptr;
  size_t _expected = 0;
  size_t _filled = 0;
};

} // namespace wheelie

#endif // WHEELIE_CLIENT_RESPONSE_PARSER_HPP
