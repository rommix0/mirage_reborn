// SpscQueue: single-producer single-consumer ring (host/include/mirage/spsc_queue.h).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <thread>

#include "mirage/spsc_queue.h"

using mirage::SpscQueue;

TEST_CASE("spsc queue fills to N, rejects N+1, pops in order", "[spsc]") {
  SpscQueue<int, 8> q;
  int out = -1;
  REQUIRE_FALSE(q.pop(out));
  REQUIRE(q.sizeApprox() == 0);
  for (int i = 0; i < 8; ++i) REQUIRE(q.push(int(i)));
  REQUIRE(q.sizeApprox() == 8);
  const int nine = 8;
  REQUIRE_FALSE(q.push(nine));
  REQUIRE_FALSE(q.push(8));
  for (int i = 0; i < 8; ++i) {
    REQUIRE(q.pop(out));
    REQUIRE(out == i);
  }
  REQUIRE_FALSE(q.pop(out));
  // The indices wrap: fill and drain again past the first lap.
  for (int lap = 0; lap < 3; ++lap) {
    for (int i = 0; i < 8; ++i) REQUIRE(q.push(lap * 10 + i));
    for (int i = 0; i < 8; ++i) {
      REQUIRE(q.pop(out));
      REQUIRE(out == lap * 10 + i);
    }
  }
}

TEST_CASE("spsc queue moves unique_ptr through without leaks", "[spsc]") {
  {
    SpscQueue<std::unique_ptr<int>, 4> q;
    for (int i = 0; i < 4; ++i) REQUIRE(q.push(std::make_unique<int>(i)));
    // A full queue leaves the rejected value with the caller.
    auto extra = std::make_unique<int>(99);
    REQUIRE_FALSE(q.push(std::move(extra)));
    REQUIRE(extra != nullptr);
    REQUIRE(*extra == 99);
    std::unique_ptr<int> out;
    REQUIRE(q.pop(out));
    REQUIRE(*out == 0);
    REQUIRE(q.push(std::move(extra)));
    REQUIRE(extra == nullptr);
    // Three items stay queued; the queue's destructor frees them.
  }
  {
    // pop moves the value out, so the queue's slot keeps no reference alive.
    auto sp = std::make_shared<int>(5);
    std::weak_ptr<int> watch = sp;
    SpscQueue<std::shared_ptr<int>, 4> q;
    REQUIRE(q.push(std::move(sp)));
    REQUIRE_FALSE(watch.expired());
    std::shared_ptr<int> out;
    REQUIRE(q.pop(out));
    out.reset();
    REQUIRE(watch.expired());
  }
}

TEST_CASE("spsc queue survives a producer/consumer thread pair", "[spsc]") {
  constexpr int kCount = 1'000'000;
  SpscQueue<int, 1024> q;
  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i) {
      while (!q.push(int(i))) std::this_thread::yield();
    }
  });
  int expected = 0;
  bool inOrder = true;
  while (expected < kCount) {
    int v;
    if (q.pop(v)) {
      if (v != expected) {
        inOrder = false;
        break;
      }
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  if (!inOrder) {  // unblock the producer so join() cannot hang
    int v;
    while (q.pop(v)) {}
  }
  producer.join();
  REQUIRE(inOrder);
  REQUIRE(expected == kCount);
}
