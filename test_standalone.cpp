/**
 * test_standalone.cpp — Standalone test for core components
 *
 * Tests Span, SpanBuffer, and LokiExporter without PHP dependency.
 * Compile: g++ -std=c++17 -Iinclude -o test_standalone test_standalone.cpp src/span.cpp src/span_buffer.cpp src/loki_exporter.cpp -lcurl -lpthread
 */

#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include "span.h"
#include "span_buffer.h"
#include "loki_exporter.h"

using namespace php_trace;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        std::cout << "  TEST: " << name << " ... "; \
        try {

#define END_TEST() \
            std::cout << "PASSED" << std::endl; \
            tests_passed++; \
        } catch (const std::exception &e) { \
            std::cout << "FAILED: " << e.what() << std::endl; \
            tests_failed++; \
        } \
    } while(0)

#define ASSERT(cond) \
    do { if (!(cond)) throw std::runtime_error("assertion failed: " #cond); } while(0)

// ===========================================================================
// Test 1: Span creation and basic properties
// ===========================================================================
void test_span_creation()
{
    TEST("Span creation and ID generation") {
        Span s = Span::create("abc123", "parent001", "TestClass::testMethod", "test-service");
        ASSERT(!s.span_id.empty());
        ASSERT(s.span_id.size() == 16);
        ASSERT(s.trace_id == "abc123");
        ASSERT(s.parent_span_id == "parent001");
        ASSERT(s.operation_name == "TestClass::testMethod");
        ASSERT(s.service_name == "test-service");
        ASSERT(s.kind == SpanKind::INTERNAL);
        ASSERT(s.status_code == SpanStatusCode::UNSET);
        ASSERT(s.duration_ns() == 0); // not yet ended
    } END_TEST();

    TEST("Span duration calculation") {
        Span s = Span::create("trace1", "", "slowOp", "svc");
        s.start_time_ns = 1'000'000'000;
        s.end_time_ns   = 1'005'000'000;
        ASSERT(s.duration_ns() == 5'000'000);
        ASSERT(s.duration_ms() == 5.0);
    } END_TEST();

    TEST("Span root detection") {
        Span root = Span::create("t", "", "root", "s");
        ASSERT(root.is_root());
        
        Span child = Span::create("t", "parent", "child", "s");
        ASSERT(!child.is_root());
    } END_TEST();

    TEST("Span attributes") {
        Span s = Span::create("t", "", "op", "s");
        s.attributes["key1"] = AttributeValue::from_string("value1");
        s.attributes["key2"] = AttributeValue::from_int(42);
        s.attributes["key3"] = AttributeValue::from_double(3.14);
        s.attributes["key4"] = AttributeValue::from_bool(true);
        
        ASSERT(s.attributes.size() == 4);
        ASSERT(s.attributes["key1"].str_val == "value1");
        ASSERT(s.attributes["key2"].int_val == 42);
        ASSERT(s.attributes["key3"].double_val == 3.14);
        ASSERT(s.attributes["key4"].bool_val == true);
    } END_TEST();

    TEST("Span ID uniqueness") {
        Span s1 = Span::create("t", "", "a", "s");
        Span s2 = Span::create("t", "", "b", "s");
        Span s3 = Span::create("t", "", "c", "s");
        ASSERT(s1.span_id != s2.span_id);
        ASSERT(s2.span_id != s3.span_id);
        ASSERT(s1.span_id != s3.span_id);
    } END_TEST();
}

// ===========================================================================
// Test 2: SpanBuffer
// ===========================================================================
void test_span_buffer()
{
    TEST("Buffer push and drain") {
        SpanBuffer buf(16);
        
        // Push 5 spans
        for (int i = 0; i < 5; i++) {
            Span s = Span::create("t" + std::to_string(i), "", "op" + std::to_string(i), "svc");
            bool ok = buf.try_push(std::move(s));
            ASSERT(ok);
        }
        
        ASSERT(buf.size() == 5);
        ASSERT(!buf.empty());
        ASSERT(buf.capacity() == 15); // 16-1 reserved
        
        // Drain
        std::vector<Span> drained;
        size_t count = buf.drain_all(drained);
        ASSERT(count == 5);
        ASSERT(drained.size() == 5);
        ASSERT(buf.empty());
    } END_TEST();

    TEST("Buffer overflow drops oldest") {
        SpanBuffer buf(4); // capacity 3 usable slots
        
        Span s1 = Span::create("t", "", "1", "s");
        Span s2 = Span::create("t", "", "2", "s");
        Span s3 = Span::create("t", "", "3", "s");
        Span s4 = Span::create("t", "", "4", "s");
        
        ASSERT(buf.try_push(std::move(s1)));
        ASSERT(buf.try_push(std::move(s2)));
        ASSERT(buf.try_push(std::move(s3)));
        ASSERT(!buf.try_push(std::move(s4))); // should fail — buffer full
        
        ASSERT(buf.total_dropped() == 1);
        ASSERT(buf.size() == 3);
    } END_TEST();

    TEST("Buffer producer/consumer threading") {
        SpanBuffer buf(1024);
        std::atomic<bool> producer_done{false};
        std::atomic<size_t> total_consumed{0};
        
        // Producer thread
        auto producer = [&]() {
            for (int i = 0; i < 500; i++) {
                Span s = Span::create("t", "", "op", "svc");
                while (!buf.try_push(std::move(s))) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            }
            producer_done = true;
        };
        
        // Consumer thread
        auto consumer = [&]() {
            std::vector<Span> batch;
            while (!producer_done || !buf.empty()) {
                buf.drain(batch, 100, 100);
                total_consumed += batch.size();
                batch.clear();
            }
        };
        
        std::thread pt(producer);
        std::thread ct(consumer);
        pt.join();
        ct.join();
        
        ASSERT(total_consumed == 500);
        ASSERT(buf.empty());
        ASSERT(buf.total_dropped() == 0);
    } END_TEST();

    TEST("Buffer shutdown") {
        SpanBuffer buf(64);
        Span s = Span::create("t", "", "op", "svc");
        buf.try_push(std::move(s));
        
        buf.shutdown();
        ASSERT(buf.is_shutdown());
        
        // Drain should still work to drain remaining
        std::vector<Span> batch;
        buf.drain_all(batch);
        ASSERT(batch.size() == 1);
    } END_TEST();
}

// ===========================================================================
// Test 3: Loki payload building (without HTTP)
// ===========================================================================
void test_loki_payload()
{
    TEST("Loki payload generation") {
        SpanBuffer buf(64);
        LokiConfig cfg;
        cfg.endpoint     = "http://localhost:3100/loki/api/v1/push";
        cfg.job_name     = "test-job";
        cfg.service_name = "test-svc";
        
        LokiExporter exporter(buf, cfg);
        
        // Create test spans
        Span s1 = Span::create("trace123", "", "Operation::one", "my-service");
        s1.start_time_ns = 1'000'000'000;
        s1.end_time_ns   = 1'000'100'000;
        s1.status_code   = SpanStatusCode::OK;
        s1.attributes["http.method"] = AttributeValue::from_string("POST");
        
        std::vector<Span> spans;
        spans.push_back(std::move(s1));
        
        // Access private method via a friend approach... 
        // For now, just verify the exporter creates OK
        std::cout << "PASSED (exporter created, payload not tested here)" << std::endl;
        tests_passed++;
    } END_TEST();

    TEST("Tempo payload generation") {
        LokiConfig cfg;
        cfg.exporter = "tempo";
        cfg.tempo_endpoint = "http://tempo:3200/api/v2/spans";
        cfg.service_name = "test-svc";

        Span span = Span::create("trace123", "", "Operation::one", "my-service");
        span.start_time_ns = 1'000'000'000;
        span.end_time_ns = 1'000'100'000;
        span.kind = SpanKind::CLIENT;
        span.status_code = SpanStatusCode::OK;
        span.attributes["http.method"] = AttributeValue::from_string("POST");

        std::string payload = build_tempo_payload({span}, cfg);
        ASSERT(payload.find("\"traceId\":\"trace123\"") != std::string::npos);
        ASSERT(payload.find("\"name\":\"Operation::one\"") != std::string::npos);
        ASSERT(payload.find("\"serviceName\":\"test-svc\"") != std::string::npos);
    } END_TEST();

    TEST("Readable span status names") {
        ASSERT(std::string(status_code_name(SpanStatusCode::OK)) == "OK");
        ASSERT(std::string(status_code_name(SpanStatusCode::ERROR)) == "ERROR");
        ASSERT(std::string(status_code_name(SpanStatusCode::UNSET)) == "UNSET");
    } END_TEST();
}

// ===========================================================================
// Test 4: ID generation
// ===========================================================================
void test_id_generation()
{
    TEST("Span ID format (16 hex chars)") {
        auto id = Span::generate_id(8);
        ASSERT(id.size() == 16);
        for (char c : id) {
            ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
    } END_TEST();
    
    TEST("Trace ID format (32 hex chars)") {
        auto id = Span::generate_id(16);
        ASSERT(id.size() == 32);
        for (char c : id) {
            ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
    } END_TEST();
}

// ===========================================================================
// Main
// ===========================================================================
int main()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "  php-trace Standalone Component Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    std::cout << "[1] Span Model" << std::endl;
    test_span_creation();
    
    std::cout << "\n[2] ID Generation" << std::endl;
    test_id_generation();
    
    std::cout << "\n[3] SpanBuffer" << std::endl;
    test_span_buffer();
    
    std::cout << "\n[4] Loki Payload" << std::endl;
    test_loki_payload();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed"
              << " (of " << (tests_passed + tests_failed) << " total)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return tests_failed == 0 ? 0 : 1;
}
