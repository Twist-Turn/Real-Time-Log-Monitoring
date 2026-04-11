/**
 * @file test_ring_buffer.cpp
 * @brief Tests for the lock-free MPSC ring buffer.
 */

#include "RingBuffer.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace logmonitor;

// ─── Test 1: Basic push/pop ───

void test_basic_push_pop() {
    RingBuffer<int, 16> rb;

    assert(rb.empty_approx());
    assert(rb.try_push(42));

    auto val = rb.try_pop();
    assert(val.has_value());
    assert(*val == 42);
    assert(rb.empty_approx());

    std::cout << "  [PASS] test_basic_push_pop\n";
}

// ─── Test 2: Pop from empty buffer ───

void test_empty_pop() {
    RingBuffer<int, 16> rb;

    auto val = rb.try_pop();
    assert(!val.has_value());

    std::cout << "  [PASS] test_empty_pop\n";
}

// ─── Test 3: Fill buffer to capacity ───

void test_full_buffer() {
    RingBuffer<int, 8> rb;

    for (int i = 0; i < 8; ++i) {
        assert(rb.try_push(i));
    }

    // Buffer is full — next push should fail
    assert(!rb.try_push(999));

    // Pop all and verify order
    for (int i = 0; i < 8; ++i) {
        auto val = rb.try_pop();
        assert(val.has_value());
        assert(*val == i);
    }

    assert(rb.empty_approx());

    std::cout << "  [PASS] test_full_buffer\n";
}

// ─── Test 4: Wrap-around ───

void test_wraparound() {
    RingBuffer<int, 4> rb;

    // Fill and drain multiple times to exercise wrap-around
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 4; ++i) {
            assert(rb.try_push(round * 100 + i));
        }
        for (int i = 0; i < 4; ++i) {
            auto val = rb.try_pop();
            assert(val.has_value());
            assert(*val == round * 100 + i);
        }
    }

    std::cout << "  [PASS] test_wraparound\n";
}

// ─── Test 5: String elements (move semantics) ───

void test_string_elements() {
    RingBuffer<std::string, 8> rb;

    assert(rb.try_push("hello"));
    assert(rb.try_push("world"));

    auto v1 = rb.try_pop();
    assert(v1.has_value() && *v1 == "hello");

    auto v2 = rb.try_pop();
    assert(v2.has_value() && *v2 == "world");

    std::cout << "  [PASS] test_string_elements\n";
}

// ─── Test 6: Multi-producer single-consumer ───

void test_mpsc() {
    constexpr std::size_t BUF_SIZE = 1024;
    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 1000;

    RingBuffer<int, BUF_SIZE> rb;
    std::atomic<int> total_pushed{0};
    std::atomic<bool> producers_done{false};

    // Producers
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&rb, &total_pushed, p]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int value = p * ITEMS_PER_PRODUCER + i;
                while (!rb.try_push(value)) {
                    std::this_thread::yield();
                }
                total_pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Consumer
    int total_popped = 0;
    std::thread consumer([&]() {
        while (true) {
            auto val = rb.try_pop();
            if (val.has_value()) {
                ++total_popped;
            } else if (producers_done.load(std::memory_order_acquire)) {
                // Drain remaining
                while (true) {
                    val = rb.try_pop();
                    if (!val.has_value()) break;
                    ++total_popped;
                }
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (auto& t : producers) t.join();
    producers_done.store(true, std::memory_order_release);
    consumer.join();

    assert(total_popped == NUM_PRODUCERS * ITEMS_PER_PRODUCER);

    std::cout << "  [PASS] test_mpsc (" << total_popped << " items)\n";
}

// ─── Test 7: size_approx and capacity ───

void test_size_and_capacity() {
    RingBuffer<int, 32> rb;
    constexpr auto cap = RingBuffer<int, 32>::capacity();
    assert(cap == 32);

    rb.try_push(1);
    rb.try_push(2);
    rb.try_push(3);
    assert(rb.size_approx() == 3);

    rb.try_pop();
    assert(rb.size_approx() == 2);

    std::cout << "  [PASS] test_size_and_capacity\n";
}

int main() {
    std::cout << "=== Ring Buffer Tests ===\n";

    test_basic_push_pop();
    test_empty_pop();
    test_full_buffer();
    test_wraparound();
    test_string_elements();
    test_mpsc();
    test_size_and_capacity();

    std::cout << "=== All Ring Buffer Tests PASSED ===\n";
    return 0;
}
