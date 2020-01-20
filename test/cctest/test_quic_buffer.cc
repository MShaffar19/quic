#include "quic/node_quic_buffer-inl.h"
#include "node_bob-inl.h"
#include "util-inl.h"
#include "uv.h"

#include "gtest/gtest.h"
#include <memory>
#include <vector>

using node::quic::QuicBuffer;
using node::quic::QuicBufferChunk;
using node::bob::Status;
using node::bob::Options;

TEST(QuicBuffer, Simple) {
  char data[100];
  memset(&data, 0, node::arraysize(data));
  uv_buf_t buf = uv_buf_init(data, node::arraysize(data));

  bool done = false;

  QuicBuffer buffer;
  buffer.push(&buf, 1, [&](int status) {
    EXPECT_EQ(0, status);
    done = true;
  });

  buffer.consume(100);
  CHECK_EQ(0, buffer.length());

  // We have to move the read head forward in order to consume
  buffer.seek(1);
  buffer.consume(100);
  CHECK_EQ(true, done);
  CHECK_EQ(0, buffer.length());
}

TEST(QuicBuffer, ConsumeMore) {
  char data[100];
  memset(&data, 0, node::arraysize(data));
  uv_buf_t buf = uv_buf_init(data, node::arraysize(data));

  bool done = false;

  QuicBuffer buffer;
  buffer.push(&buf, 1, [&](int status) {
    EXPECT_EQ(0, status);
    done = true;
  });

  buffer.seek(1);
  buffer.consume(150);  // Consume more than what was buffered
  CHECK_EQ(true, done);
  CHECK_EQ(0, buffer.length());
}

TEST(QuicBuffer, Multiple) {
  uv_buf_t bufs[] {
    uv_buf_init("abcdefghijklmnopqrstuvwxyz", 26),
    uv_buf_init("zyxwvutsrqponmlkjihgfedcba", 26)
  };

  QuicBuffer buf;
  bool done = false;
  buf.push(bufs, 2, [&](int status) { done = true; });

  buf.seek(2);
  CHECK_EQ(buf.remaining(), 50);
  CHECK_EQ(buf.length(), 52);

  buf.consume(25);
  CHECK_EQ(buf.length(), 27);

  buf.consume(25);
  CHECK_EQ(buf.length(), 2);

  buf.consume(2);
  CHECK_EQ(0, buf.length());
}

TEST(QuicBuffer, Multiple2) {
  char* ptr = new char[100];
  memset(ptr, 0, 50);
  memset(ptr + 50, 1, 50);

  uv_buf_t bufs[] = {
    uv_buf_init(ptr, 50),
    uv_buf_init(ptr + 50, 50)
  };

  int count = 0;

  QuicBuffer buffer;
  buffer.push(
      bufs, node::arraysize(bufs),
      [&](int status) {
    count++;
    CHECK_EQ(0, status);
    delete[] ptr;
  });
  buffer.seek(node::arraysize(bufs));

  buffer.consume(25);
  CHECK_EQ(75, buffer.length());
  buffer.consume(25);
  CHECK_EQ(50, buffer.length());
  buffer.consume(25);
  CHECK_EQ(25, buffer.length());
  buffer.consume(25);
  CHECK_EQ(0, buffer.length());

  // The callback was only called once tho
  CHECK_EQ(1, count);
}

TEST(QuicBuffer, Cancel) {
  char* ptr = new char[100];
  memset(ptr, 0, 50);
  memset(ptr + 50, 1, 50);

  uv_buf_t bufs[] = {
    uv_buf_init(ptr, 50),
    uv_buf_init(ptr + 50, 50)
  };

  int count = 0;

  QuicBuffer buffer;
  buffer.push(
      bufs, node::arraysize(bufs),
      [&](int status) {
    count++;
    CHECK_EQ(UV_ECANCELED, status);
    delete[] ptr;
  });

  buffer.seek(1);
  buffer.consume(25);
  CHECK_EQ(75, buffer.length());
  buffer.cancel();
  CHECK_EQ(0, buffer.length());

  // The callback was only called once tho
  CHECK_EQ(1, count);
}

TEST(QuicBuffer, Move) {
  QuicBuffer buffer1;
  QuicBuffer buffer2;

  char data[100];
  memset(&data, 0, node::arraysize(data));
  uv_buf_t buf = uv_buf_init(data, node::arraysize(data));

  buffer1.push(&buf, 1);

  CHECK_EQ(100, buffer1.length());

  buffer2 = std::move(buffer1);
  CHECK_EQ(0, buffer1.length());
  CHECK_EQ(100, buffer2.length());
}

TEST(QuicBuffer, QuicBufferChunk) {
  std::unique_ptr<QuicBufferChunk> chunk =
      std::make_unique<QuicBufferChunk>(100);
  memset(chunk->out(), 1, 100);

  QuicBuffer buffer;
  buffer.push(std::move(chunk));
  buffer.end();
  CHECK_EQ(100, buffer.length());

  auto next = [&](
      int status,
      const ngtcp2_vec* data,
      size_t count,
      QuicBuffer::Done done) {
    CHECK_EQ(status, Status::STATUS_END);
    CHECK_EQ(count, 1);
    CHECK_NOT_NULL(data);
    done(100);
  };

  CHECK_EQ(buffer.remaining(), 100);

  ngtcp2_vec data[2];
  size_t len = sizeof(data) / sizeof(ngtcp2_vec);
  buffer.pull(next, Options::OPTIONS_SYNC | Options::OPTIONS_END, data, len);

  CHECK_EQ(buffer.remaining(), 0);

  buffer.consume(50);
  CHECK_EQ(50, buffer.length());

  buffer.consume(50);
  CHECK_EQ(0, buffer.length());
}
