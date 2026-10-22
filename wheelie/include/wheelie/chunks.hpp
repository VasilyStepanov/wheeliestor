/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIE_CHUNKS_HPP
#define WHEELIE_CHUNKS_HPP

#include <wheelio/reactor.hpp>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace wheelie {

/*
 * Where the bytes of a connection come from: the two ways of receiving them,
 * which a server and a client both offer, behind the same
 * `while (auto chunk = co_await chunks.next())`. A chunk is a view that is
 * good until the coroutine suspends again, and the end of the stream is
 * std::nullopt.
 */

// Receive buffers: a chunk is about as much as one receive returns. A small
// ring is better than a large one: its few buffers are reused while they are
// still in the cache.
inline constexpr size_t recv_entry_size = 128 * 1024;
inline constexpr unsigned int recv_entries = 4;

// The chunks of a connection, received one receive at a time.
class SingleShotChunks final {
private:
  wheelio::Reactor &_reactor;
  int _fd;
  std::unique_ptr<std::byte[]> _buffer;

public:
  SingleShotChunks(wheelio::Reactor &reactor, int fd)
      : _reactor(reactor), _fd(fd),
        _buffer(std::make_unique<std::byte[]>(recv_entry_size)) {}

  class Next final {
  private:
    wheelio::Operation<size_t> _operation;
    const std::byte *_data;

  public:
    explicit Next(SingleShotChunks &chunks)
        : _operation(chunks._reactor.recv(chunks._fd, chunks._buffer.get(),
                                          recv_entry_size)),
          _data(chunks._buffer.get()) {}

    bool await_ready() const noexcept { return _operation.await_ready(); }

    void await_suspend(std::coroutine_handle<> h) noexcept {
      _operation.await_suspend(h);
    }

    std::optional<std::span<const std::byte>> await_resume() {
      size_t received = _operation.await_resume();
      if (received == 0) {
        return std::nullopt;
      }
      return std::span<const std::byte>(_data, received);
    }
  };

  Next next() { return Next(*this); }
};

// The chunks of a connection, received by one multishot receive.
class MultishotChunks final {
private:
  wheelio::RecvStream _stream;

public:
  MultishotChunks(wheelio::Reactor &reactor, int fd)
      : _stream(reactor.recv_multishot(fd, recv_entry_size, recv_entries)) {}

  wheelio::RecvStream::Next next() { return _stream.next(); }
};

} // namespace wheelie

#endif // WHEELIE_CHUNKS_HPP
