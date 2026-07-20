#include <gtest/gtest.h>
#include <sink.h>
#include <zmq.hpp>

// For debugging remember to call
// set target.disable-aslr false
// on lldb
TEST(Sink, CanCompile) {
  zmq::context_t ctx;
  [[maybe_unused]] auto s = sink::Sink(ctx, "1.1.1.1:2000");
}
