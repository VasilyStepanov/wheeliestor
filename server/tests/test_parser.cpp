#include <server/request_parser.hpp>

#include <wheelie/protocol.h>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using wheelie::AlignedBuffer;
using wheelie::ProtocolError;
using wheelie::Request;
using wheelie::RequestParser;

namespace {

std::vector<std::byte> encode(uint64_t cid, uint8_t op, uint64_t offset,
                              uint64_t size, const std::vector<char> &payload,
                              uint8_t flags = 0) {
  WheelieRequestHeader header = {cid, offset, size, op, flags};
  std::vector<std::byte> bytes(sizeof(header) + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
  return bytes;
}

std::vector<char> pattern(size_t size, char seed) {
  std::vector<char> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>(seed + i % 31);
  }
  return data;
}

// Feeds `bytes` in pieces of `piece` bytes and collects the requests.
std::vector<Request> parse(RequestParser &parser,
                           const std::vector<std::byte> &bytes, size_t piece) {
  std::vector<Request> requests;
  for (size_t at = 0; at < bytes.size(); at += piece) {
    size_t take = std::min(piece, bytes.size() - at);
    parser.feed(
        std::span<const std::byte>(bytes.data() + at, take),
        [&](Request &&request) { requests.push_back(std::move(request)); });
  }
  return requests;
}

} // namespace

TEST(AlignedBuffer, IsAlignedAndMovable) {
  AlignedBuffer buffer(5000);
  EXPECT_EQ(buffer.size(), 5000u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(buffer.data()) %
                AlignedBuffer::alignment,
            0u);

  const std::byte *data = buffer.data();
  AlignedBuffer moved = std::move(buffer);
  EXPECT_EQ(moved.data(), data);
  EXPECT_EQ(buffer.data(), nullptr);
  EXPECT_EQ(buffer.size(), 0u);
}

TEST(AlignedBuffer, EmptyHasNoData) {
  AlignedBuffer buffer(0);
  EXPECT_EQ(buffer.size(), 0u);
  EXPECT_EQ(buffer.data(), nullptr);
}

TEST(RequestParser, ReadHasNoPayload) {
  RequestParser parser;
  auto requests =
      parse(parser, encode(7, WHEELIE_OP_READ, 4096, 8192, {}), 1000);

  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].header.cid, 7u);
  EXPECT_EQ(requests[0].header.op, WHEELIE_OP_READ);
  EXPECT_EQ(requests[0].header.offset, 4096u);
  EXPECT_EQ(requests[0].header.size, 8192u);
  EXPECT_EQ(requests[0].payload.size(), 0u);
  EXPECT_FALSE(parser.mid_request());
}

TEST(RequestParser, WriteCarriesItsPayload) {
  RequestParser parser;
  std::vector<char> payload = pattern(4096, 'a');
  auto requests = parse(parser,
                        encode(9, WHEELIE_OP_WRITE, 512, payload.size(),
                               payload, WHEELIE_FLAG_SYNC),
                        10000);

  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].header.op, WHEELIE_OP_WRITE);
  EXPECT_EQ(requests[0].header.flags, WHEELIE_FLAG_SYNC);
  ASSERT_EQ(requests[0].payload.size(), payload.size());
  EXPECT_EQ(
      std::memcmp(requests[0].payload.data(), payload.data(), payload.size()),
      0);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(requests[0].payload.data()) %
                AlignedBuffer::alignment,
            0u);
}

TEST(RequestParser, PiecesOfAnySizeGiveTheSameRequests) {
  std::vector<char> first = pattern(300, 'x');
  std::vector<char> second = pattern(1, 'y');
  std::vector<std::byte> stream = encode(1, WHEELIE_OP_WRITE, 0, 300, first);
  std::vector<std::byte> read = encode(2, WHEELIE_OP_READ, 77, 64, {});
  std::vector<std::byte> small = encode(3, WHEELIE_OP_WRITE, 5, 1, second);
  stream.insert(stream.end(), read.begin(), read.end());
  stream.insert(stream.end(), small.begin(), small.end());

  for (size_t piece : {1u, 2u, 7u, 25u, 26u, 27u, 100u, 5000u}) {
    RequestParser parser;
    auto requests = parse(parser, stream, piece);
    ASSERT_EQ(requests.size(), 3u) << piece;
    EXPECT_EQ(requests[0].header.cid, 1u) << piece;
    EXPECT_EQ(std::memcmp(requests[0].payload.data(), first.data(), 300), 0)
        << piece;
    EXPECT_EQ(requests[1].header.cid, 2u) << piece;
    EXPECT_EQ(requests[1].header.offset, 77u) << piece;
    EXPECT_EQ(requests[2].header.cid, 3u) << piece;
    EXPECT_EQ(requests[2].payload.size(), 1u) << piece;
    EXPECT_FALSE(parser.mid_request()) << piece;
  }
}

TEST(RequestParser, WriteOfNothingIsComplete) {
  RequestParser parser;
  auto requests = parse(parser, encode(4, WHEELIE_OP_WRITE, 0, 0, {}), 8);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].payload.size(), 0u);
}

TEST(RequestParser, KnowsWhenARequestIsUnfinished) {
  RequestParser parser;
  std::vector<std::byte> bytes =
      encode(5, WHEELIE_OP_WRITE, 0, 100, pattern(100, 'z'));

  std::vector<std::byte> partial(bytes.begin(), bytes.begin() + 10);
  EXPECT_TRUE(parse(parser, partial, 100).empty());
  EXPECT_TRUE(parser.mid_request());

  std::vector<std::byte> in_payload(bytes.begin() + 10, bytes.begin() + 60);
  EXPECT_TRUE(parse(parser, in_payload, 100).empty());
  EXPECT_TRUE(parser.mid_request());

  std::vector<std::byte> rest(bytes.begin() + 60, bytes.end());
  EXPECT_EQ(parse(parser, rest, 100).size(), 1u);
  EXPECT_FALSE(parser.mid_request());
}

TEST(RequestParser, RejectsAnUnknownOperation) {
  RequestParser parser;
  EXPECT_THROW(parse(parser, encode(6, 9, 0, 0, {}), 100), ProtocolError);
}

TEST(RequestParser, FlagsAreEmptyUnlessAsked) {
  RequestParser parser;
  auto requests = parse(parser, encode(1, WHEELIE_OP_WRITE, 0, 0, {}), 100);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].header.flags, 0u);
}

TEST(RequestParser, RejectsAnUnknownFlag) {
  RequestParser parser;
  EXPECT_THROW(
      parse(parser,
            encode(2, WHEELIE_OP_WRITE, 0, 0, {}, WHEELIE_FLAG_SYNC | 0x80),
            100),
      ProtocolError);
}

TEST(RequestParser, RejectsAFlagThatDoesNotApplyToARead) {
  RequestParser parser;
  EXPECT_THROW(parse(parser,
                     encode(3, WHEELIE_OP_READ, 0, 4096, {}, WHEELIE_FLAG_SYNC),
                     100),
               ProtocolError);
}

TEST(RequestParser, RejectsAWriteLargerThanTheMaximum) {
  RequestParser parser;
  EXPECT_THROW(
      parse(parser,
            encode(8, WHEELIE_OP_WRITE, 0, WHEELIE_MAX_PAYLOAD_SIZE + 1, {}),
            100),
      ProtocolError);
}

TEST(RequestParser, AReadMayAskForMoreThanTheMaximum) {
  // It is answered with an error, not a reason to drop the connection: a read
  // has no payload to lose track of.
  RequestParser parser;
  auto requests = parse(
      parser, encode(10, WHEELIE_OP_READ, 0, WHEELIE_MAX_PAYLOAD_SIZE + 1, {}),
      100);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].header.size, WHEELIE_MAX_PAYLOAD_SIZE + 1);
}
