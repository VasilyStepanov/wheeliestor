#ifndef WHEELIE_SERVER_ALIGNED_BUFFER_HPP
#define WHEELIE_SERVER_ALIGNED_BUFFER_HPP

#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

namespace wheelie {

// A buffer aligned to a page, as a file opened with O_DIRECT needs it.
class AlignedBuffer final {
public:
  static constexpr size_t alignment = 4096;

  AlignedBuffer() = default;

  explicit AlignedBuffer(size_t size) : _size(size) {
    if (size == 0) {
      return;
    }
    void *data = nullptr;
    if (posix_memalign(&data, alignment, size) != 0) {
      throw std::bad_alloc();
    }
    _data = static_cast<std::byte *>(data);
  }

  AlignedBuffer(const AlignedBuffer &) = delete;
  AlignedBuffer &operator=(const AlignedBuffer &) = delete;

  AlignedBuffer(AlignedBuffer &&other) noexcept
      : _data(std::exchange(other._data, nullptr)),
        _size(std::exchange(other._size, 0)) {}

  AlignedBuffer &operator=(AlignedBuffer &&other) noexcept {
    if (this != &other) {
      std::free(_data);
      _data = std::exchange(other._data, nullptr);
      _size = std::exchange(other._size, 0);
    }
    return *this;
  }

  ~AlignedBuffer() { std::free(_data); }

  std::byte *data() noexcept { return _data; }
  const std::byte *data() const noexcept { return _data; }
  size_t size() const noexcept { return _size; }

private:
  std::byte *_data = nullptr;
  size_t _size = 0;
};

} // namespace wheelie

#endif // WHEELIE_SERVER_ALIGNED_BUFFER_HPP
