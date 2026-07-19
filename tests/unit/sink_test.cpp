#include <gtest/gtest.h>
#include <sink.h>
#include <zmq.hpp>

TEST(Sink, CanCompile) {
  zmq::context_t ctx;
  sink::Sink(ctx, "1.1.1.1:2000");
}
