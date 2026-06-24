/**
 * span_buffer.cpp - Thread-safe ring buffer implementation
 */

#include "span_buffer.h"
#include <cstring>
#include <thread>

namespace php_trace {

SpanBuffer::SpanBuffer(size_t capacity)
    : capacity_(capacity)
    , slots_(new Slot[capacity])
    , write_pos_(0)
    , read_pos_(0)
{
}

SpanBuffer::~SpanBuffer() = default;

// ---------------------------------------------------------------------------
// Producer side: push (called from PHP worker threads)
// ---------------------------------------------------------------------------
bool SpanBuffer::try_push(Span &&span)
{
    std::lock_guard<std::mutex> lock(producer_mutex_);
    
    size_t w = write_pos_.load(std::memory_order_relaxed);
    size_t next = (w + 1) % capacity_;
    
    // If the next slot is being read, the buffer is full
    if (next == read_pos_) {
        total_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false; // drop
    }
    
    // Write the span
    slots_[w].span = std::move(span);
    slots_[w].has_data.store(true, std::memory_order_release);
    write_pos_.store(next, std::memory_order_release);
    
    total_pushed_.fetch_add(1, std::memory_order_relaxed);
    
    // Notify consumer
    consumer_cv_.notify_one();
    
    return true;
}

bool SpanBuffer::push(Span &&span, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(producer_mutex_);
    
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    
    while (true) {
        size_t w = write_pos_.load(std::memory_order_relaxed);
        size_t next = (w + 1) % capacity_;
        
        if (next != read_pos_) {
            // Space available
            slots_[w].span = std::move(span);
            slots_[w].has_data.store(true, std::memory_order_release);
            write_pos_.store(next, std::memory_order_release);
            total_pushed_.fetch_add(1, std::memory_order_relaxed);
            consumer_cv_.notify_one();
            return true;
        }
        
        if (std::chrono::steady_clock::now() >= deadline) {
            total_dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        lock.lock();
    }
}

// ---------------------------------------------------------------------------
// Consumer side: drain (called from exporter thread)
// ---------------------------------------------------------------------------
size_t SpanBuffer::drain(std::vector<Span> &out, size_t max_count, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(consumer_mutex_);
    
    // Wait for data or shutdown
    consumer_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
        return read_pos_ != write_pos_.load(std::memory_order_acquire) || shutdown_.load(std::memory_order_acquire);
    });
    
    if (shutdown_.load(std::memory_order_acquire)) {
        // Drain whatever remains
    }
    
    size_t drained = 0;
    while (drained < max_count && read_pos_ != write_pos_.load(std::memory_order_acquire)) {
        auto &slot = slots_[read_pos_];
        
        // Wait until the producer has finished writing
        // (necessary if write and has_data.store are separated)
        while (!slot.has_data.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        
        out.push_back(std::move(slot.span));
        slot.has_data.store(false, std::memory_order_release);
        
        read_pos_ = (read_pos_ + 1) % capacity_;
        ++drained;
    }
    
    total_drained_.fetch_add(drained, std::memory_order_relaxed);
    return drained;
}

size_t SpanBuffer::drain_all(std::vector<Span> &out)
{
    std::unique_lock<std::mutex> lock(consumer_mutex_);
    
    size_t drained = 0;
    while (read_pos_ != write_pos_.load(std::memory_order_acquire)) {
        auto &slot = slots_[read_pos_];
        
        while (!slot.has_data.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        
        out.push_back(std::move(slot.span));
        slot.has_data.store(false, std::memory_order_release);
        
        read_pos_ = (read_pos_ + 1) % capacity_;
        ++drained;
    }
    
    total_drained_.fetch_add(drained, std::memory_order_relaxed);
    return drained;
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
size_t SpanBuffer::size() const
{
    size_t w = write_pos_.load(std::memory_order_acquire);
    if (w >= read_pos_) return w - read_pos_;
    return capacity_ - read_pos_ + w;
}

size_t SpanBuffer::capacity() const { return capacity_ - 1; } // one slot reserved
bool SpanBuffer::empty() const { return read_pos_ == write_pos_.load(std::memory_order_acquire); }

uint64_t SpanBuffer::total_pushed() const  { return total_pushed_.load(std::memory_order_relaxed); }
uint64_t SpanBuffer::total_dropped() const { return total_dropped_.load(std::memory_order_relaxed); }
uint64_t SpanBuffer::total_drained() const { return total_drained_.load(std::memory_order_relaxed); }

void SpanBuffer::shutdown() 
{
    shutdown_.store(true, std::memory_order_release);
    consumer_cv_.notify_all();
}

bool SpanBuffer::is_shutdown() const
{
    return shutdown_.load(std::memory_order_acquire);
}

} // namespace php_trace
