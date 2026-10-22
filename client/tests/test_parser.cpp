#include <client/response_parser.hpp>

#include <wheelie/protocol.h>

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <vector>

using wheelie::ProtocolError;
using wheelie::ResponseParser;
using wheelie::ResponseTarget;

namespace {

std::vector<std::byte> encode(uint64_t cid, int64_t res,
                              const std::vector<char> &payload = {}) {
  WheelieResponseHeader header = {cid, res};
  std::vector<std::byte> bytes(sizeof(header) + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
  return bytes;
}

std::vector<char> pattern(size_t size, char seed) {
  std::vector<char> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>(seed + i % 29);
  }
  return data;
}

// The requests in flight: reads have a buffer to fill, writes have none.
class Requests {
public:
  void add_read(uint64_t cid, size_t size) {
    buffers_[cid].assign(size, std::byte{0});
    targets_[cid] = ResponseTarget{buffers_[cid].data(), size};
  }

  void add_write(uint64_t cid) { targets_[cid] = ResponseTarget{nullptr, 0}; }

  std::optional<ResponseTarget> lookup(uint64_t cid) const {
    auto found = targets_.find(cid);
    if (found == targets_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  const std::vector<std::byte> &buffer(uint64_t cid) { return buffers_[cid]; }

private:
  std::map<uint64_t, ResponseTarget> targets_;
  std::map<uint64_t, std::vector<std::byte>> buffers_;
};

struct Done {
  uint64_t cid;
  int64_t res;
};

std::vector<Done> parse(ResponseParser &parser, Requests &requests,
                        const std::vector<std::byte> &bytes, size_t piece) {
  std::vector<Done> done;
  for (size_t at = 0; at < bytes.size(); at += piece) {
    size_t take = std::min(piece, bytes.size() - at);
    parser.feed(
        std::span<const std::byte>(bytes.data() + at, take),
        [&](uint64_t cid) { return requests.lookup(cid); },
        [&](uint64_t cid, int64_t res) { done.push_back({cid, res}); });
  }
  return done;
}

} // namespace

TEST(ResponseParser, ReadDataGoesIntoTheBufferOfItsRequest) {
  Requests requests;
  requests.add_read(3, 4096);
  ResponseParser parser;
  std::vector<char> data = pattern(4096, 'a');

  auto done = parse(parser, requests, encode(3, 4096, data), 700);
  ASSERT_EQ(done.size(), 1u);
  EXPECT_EQ(done[0].cid, 3u);
  EXPECT_EQ(done[0].res, 4096);
  EXPECT_EQ(std::memcmp(requests.buffer(3).data(), data.data(), 4096), 0);
  EXPECT_FALSE(parser.mid_response());
}

TEST(ResponseParser, ShortReadFillsOnlyWhatCame) {
  Requests requests;
  requests.add_read(1, 4096);
  ResponseParser parser;
  std::vector<char> data = pattern(100, 'b');

  auto done = parse(parser, requests, encode(1, 100, data), 4096);
  ASSERT_EQ(done.size(), 1u);
  EXPECT_EQ(done[0].res, 100);
  EXPECT_EQ(std::memcmp(requests.buffer(1).data(), data.data(), 100), 0);
  EXPECT_EQ(requests.buffer(1)[100], std::byte{0});
}

TEST(ResponseParser, WriteResponseHasNoData) {
  Requests requests;
  requests.add_write(8);
  ResponseParser parser;

  auto done = parse(parser, requests, encode(8, 4096), 5);
  ASSERT_EQ(done.size(), 1u);
  EXPECT_EQ(done[0].cid, 8u);
  EXPECT_EQ(done[0].res, 4096);
}

TEST(ResponseParser, ErrorAndEmptyReadHaveNoData) {
  Requests requests;
  requests.add_read(1, 4096);
  requests.add_read(2, 4096);
  ResponseParser parser;

  std::vector<std::byte> stream = encode(1, -EINVAL);
  std::vector<std::byte> empty = encode(2, 0);
  stream.insert(stream.end(), empty.begin(), empty.end());

  auto done = parse(parser, requests, stream, 7);
  ASSERT_EQ(done.size(), 2u);
  EXPECT_EQ(done[0].res, -EINVAL);
  EXPECT_EQ(done[1].res, 0);
}

TEST(ResponseParser, PiecesOfAnySizeGiveTheSameResponses) {
  std::vector<char> first = pattern(300, 'x');
  std::vector<char> third = pattern(1, 'y');
  std::vector<std::byte> stream = encode(1, 300, first);
  std::vector<std::byte> second = encode(2, 4096);
  std::vector<std::byte> last = encode(3, 1, third);
  stream.insert(stream.end(), second.begin(), second.end());
  stream.insert(stream.end(), last.begin(), last.end());

  for (size_t piece : {1u, 2u, 7u, 15u, 16u, 17u, 100u, 5000u}) {
    Requests requests;
    requests.add_read(1, 512);
    requests.add_write(2);
    requests.add_read(3, 8);
    ResponseParser parser;

    auto done = parse(parser, requests, stream, piece);
    ASSERT_EQ(done.size(), 3u) << piece;
    EXPECT_EQ(done[0].cid, 1u) << piece;
    EXPECT_EQ(done[1].cid, 2u) << piece;
    EXPECT_EQ(done[2].cid, 3u) << piece;
    EXPECT_EQ(std::memcmp(requests.buffer(1).data(), first.data(), 300), 0)
        << piece;
    EXPECT_EQ(requests.buffer(3)[0], std::byte{'y'}) << piece;
    EXPECT_FALSE(parser.mid_response()) << piece;
  }
}

TEST(ResponseParser, KnowsWhenAResponseIsUnfinished) {
  Requests requests;
  requests.add_read(1, 100);
  ResponseParser parser;
  std::vector<std::byte> bytes = encode(1, 100, pattern(100, 'z'));

  std::vector<std::byte> partial(bytes.begin(), bytes.begin() + 5);
  EXPECT_TRUE(parse(parser, requests, partial, 100).empty());
  EXPECT_TRUE(parser.mid_response());

  std::vector<std::byte> in_payload(bytes.begin() + 5, bytes.begin() + 60);
  EXPECT_TRUE(parse(parser, requests, in_payload, 100).empty());
  EXPECT_TRUE(parser.mid_response());

  std::vector<std::byte> rest(bytes.begin() + 60, bytes.end());
  EXPECT_EQ(parse(parser, requests, rest, 100).size(), 1u);
  EXPECT_FALSE(parser.mid_response());
}

TEST(ResponseParser, RejectsAResponseToNothing) {
  Requests requests;
  ResponseParser parser;
  EXPECT_THROW(parse(parser, requests, encode(5, 0), 100), ProtocolError);
}

TEST(ResponseParser, RejectsMoreDataThanWasAskedFor) {
  Requests requests;
  requests.add_read(1, 100);
  ResponseParser parser;
  EXPECT_THROW(parse(parser, requests, encode(1, 101, pattern(101, 'q')), 100),
               ProtocolError);
}
