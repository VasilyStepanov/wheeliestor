#include <wheelio/wheelio.h>

#include <gtest/gtest.h>

namespace {

struct Request {
  int id;
  WioOp op;
  char payload[16];
};

} // namespace

TEST(Api, ContainerOfFindsEmbeddingStruct) {
  Request request = {};
  EXPECT_EQ(wio_container_of(&request.op, Request, op), &request);
}
