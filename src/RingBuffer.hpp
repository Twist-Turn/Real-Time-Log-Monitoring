#pragma once
/**
 * @file RingBuffer.hpp
 * @brief Lock-free MPSC (Multiple Producer, Single Consumer) ring buffer.
 *
 * Uses per-slot sequence counters with CAS operations for producers and
 * simple sequence checks for the single consumer. Zero allocations on the
 * hot path. The capacity MUST be a power of two for fast modulo via bitmask.
 *
 * Design:
 *   - Each slot has a sequence number that advances through states:
 *       sequence == index        → slot is EMPTY, ready for a producer
 *       sequence == index + 1    → slot is FULL, ready for the consumer
 *   - Producers: CAS(slot.sequence, expected=pos, desired=pos+1) to claim,
 *     then write data, then store(sequence = pos + 1) to publish.
 *   - Consumer: reads slot.sequence; if == tail + 1, data is ready. After
 *     reading, stores sequence = tail + capacity to recycle the slot.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>       // std::hardware_destructive_interference_size
#include <optional>
#include <type_traits>

namespace logmonitor {

// Cache line size fallback for platforms that don't define it
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t kCacheLineSize = 64;
#endif

/**
 * @tparam T        Element type (must be move-constructible)
 * @tparam Capacity Must be a power of two
 */
template <typename T, std::size_t Capacity>
class RingBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "RingBuffer capacity must be a power of two");
    static_assert(std::is_move_constructible_v<T>,
                  "RingBuffer element type must be move-constructible");

public:
    RingBuffer() noexcept {
        // Initialize every slot's sequence to its index (EMPTY state)
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // Non-copyable, non-movable (shared across threads)
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /**
     * @brief Try to push an element (producer side, thread-safe for multiple producers).
     * @return true if enqueued, false if buffer is full.
     *
     * Lock-free: uses CAS loop on the head position. Wait-free for the
     * common case where contention is low (CAS succeeds on first try).
     */
    bool try_push(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        std::size_t pos = head_.load(std::memory_order_relaxed);

        for (;;) {
            auto& slot = slots_[pos & kMask];
            std::size_t seq = slot.sequence.load(std::memory_order_acquire);

            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                // Slot is empty and matches our position — try to claim it
                if (head_.compare_exchange_weak(pos, pos + 1,
                                                 std::memory_order_relaxed)) {
                    // We own this slot. Write the data then publish.
                    slot.storage.construct(std::move(value));
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed — another producer claimed it. pos was updated by CAS.
            } else if (diff < 0) {
                // Buffer is full (consumer hasn't freed this slot yet)
                return false;
            } else {
                // Another producer already claimed this slot but we haven't
                // advanced head_ yet. Reload head_ and retry.
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * @brief Try to pop an element (consumer side, single consumer only).
     * @return The element if available, std::nullopt if buffer is empty.
     *
     * Wait-free: single consumer never contends.
     */
    std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
        auto& slot = slots_[tail_ & kMask];
        std::size_t seq = slot.sequence.load(std::memory_order_acquire);

        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail_ + 1);

        if (diff == 0) {
            // Data is ready — read it
            T value = slot.storage.move_and_destroy();
            // Recycle the slot: advance its sequence by Capacity so producers
            // can reuse it after wrapping around
            slot.sequence.store(tail_ + Capacity, std::memory_order_release);
            ++tail_;
            return value;
        }

        // Buffer is empty (data not yet published)
        return std::nullopt;
    }

    /// Approximate number of elements in the buffer (racy but useful for stats)
    [[nodiscard]] std::size_t size_approx() const noexcept {
        std::size_t h = head_.load(std::memory_order_relaxed);
        std::size_t t = tail_;  // Only consumer reads tail_, so no atomic needed here
        return h >= t ? h - t : 0;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    /**
     * In-place storage for T without default-constructing every slot at startup.
     * Provides explicit construct/destroy lifecycle management.
     */
    struct Storage {
        typename std::aligned_storage<sizeof(T), alignof(T)>::type data;

        template <typename... Args>
        void construct(Args&&... args) {
            ::new (&data) T(std::forward<Args>(args)...);
        }

        T move_and_destroy() {
            T* ptr = reinterpret_cast<T*>(&data);
            T val = std::move(*ptr);
            ptr->~T();
            return val;
        }
    };

    /// One slot in the ring buffer: sequence counter + data storage.
    struct alignas(kCacheLineSize) Slot {
        std::atomic<std::size_t> sequence{0};
        Storage storage;
    };

    // Head (write position) — shared among all producers, cache-line aligned
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};

    // Tail (read position) — only accessed by the single consumer
    alignas(kCacheLineSize) std::size_t tail_{0};

    // The slot array
    Slot slots_[Capacity];
};

} // namespace logmonitor
