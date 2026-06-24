/**
 * span_buffer.h - Thread-Safe Lock-Free Ring Buffer for Spans
 *
 * Uses a bounded SPSC (Single Producer, Single Consumer) ring buffer.
 * For the multi-producer case (multiple PHP workers), we use a mutex.
 *
 * Strategy:
 *   - Each PHP worker thread pushes completed spans.
 *   - A single background exporter thread drains them in batches.
 *   - If the buffer is full, the oldest span is silently dropped.
 */

#ifndef PHP_TRACE_SPAN_BUFFER_H
#define PHP_TRACE_SPAN_BUFFER_H

#include "span.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace php_trace {

class SpanBuffer {
public:
    explicit SpanBuffer(size_t capacity = 65536);
    ~SpanBuffer();

    // ---- producer API (called from PHP worker threads) ----

    /**
     * Try to push a span. Returns true on success.
     * If the buffer is full, the span is dropped and false is returned.
     * This is intentionally lossy to avoid blocking PHP request processing.
     */
    bool try_push(Span &&span);

    /**
     * Push a span, blocking up to timeout_ms.
     * Returns true on success, false on timeout or full.
     */
    bool push(Span &&span, int timeout_ms = 100);

    // ---- consumer API (called from exporter thread) ----

    /**
     * Drain up to `max_count` spans into `out`.
     * Returns the number of spans drained.
     * Blocks up to timeout_ms if buffer is empty.
     */
    size_t drain(std::vector<Span> &out, size_t max_count, int timeout_ms = 5000);

    /**
     * Drain ALL available spans (non-blocking) into `out`.
     * Returns the number of spans drained.
     */
    size_t drain_all(std::vector<Span> &out);

    // ---- stats ----

    size_t size() const;
    size_t capacity() const;
    bool empty() const;
    uint64_t total_pushed() const;
    uint64_t total_dropped() const;
    uint64_t total_drained() const;

    // ---- shutdown ----

    /** Signal the drain side to stop waiting. */
    void shutdown();
    bool is_shutdown() const;

private:
    struct alignas(64) Slot {
        std::atomic<bool> has_data{false};
        Span span;
    };

    size_t              capacity_;
    std::unique_ptr<Slot[]> slots_;

    // Producer position (written only by producers under mutex)
    std::atomic<size_t> write_pos_;
    // Consumer position (written only by consumer)
    size_t              read_pos_;

    // Mutex for multi-producer safety
    mutable std::mutex  producer_mutex_;

    // CV for consumer to wait on
    std::mutex              consumer_mutex_;
    std::condition_variable consumer_cv_;

    // Stats
    std::atomic<uint64_t> total_pushed_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_drained_{0};

    // Shutdown flag
    std::atomic<bool> shutdown_{false};
};

} // namespace php_trace

#endif // PHP_TRACE_SPAN_BUFFER_H
