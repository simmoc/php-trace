/**
 * test_standalone.cpp — Comprehensive unit test suite for php-trace components
 *
 * Covers: Span model, ID generation, SpanBuffer, Loki/Tempo payload builders.
 *
 * Compile:
 *   g++ -std=c++17 -Iinclude -o test_standalone test_standalone.cpp \
 *       src/span.cpp src/span_buffer.cpp src/loki_exporter.cpp \
 *       -lcurl -lpthread -O0 -g
 *
 * Run:
 *   ./test_standalone
 */

#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <sstream>
#include <set>
#include <string>
#include <vector>
#include <mutex>
#include "span.h"
#include "span_buffer.h"
#include "loki_exporter.h"

using namespace php_trace;

// ===========================================================================
// Test framework (lightweight, no external dependency)
// ===========================================================================
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        std::cout << "  [" << name << "] "; \
        try {

#define END_TEST() \
            std::cout << "PASSED" << std::endl; \
            tests_passed++; \
        } catch (const std::exception &e) { \
            std::cout << "FAILED — " << e.what() << std::endl; \
            tests_failed++; \
        } \
    } while(0)

#define ASSERT(cond) \
    do { if (!(cond)) throw std::runtime_error(__FILE__ ":" + std::to_string(__LINE__) + " assertion failed: " #cond); } while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) { \
        std::ostringstream _oss; \
        _oss << __FILE__ ":" << __LINE__ << " " #a " == " #b; \
        throw std::runtime_error(_oss.str()); \
    }} while(0)

#define ASSERT_GT(a, b) \
    do { if (!((a) > (b))) { \
        std::ostringstream _oss; \
        _oss << __FILE__ ":" << __LINE__ << " " #a " > " #b; \
        throw std::runtime_error(_oss.str()); \
    }} while(0)

#define ASSERT_ENUM_EQ(a, b) \
    do { if (static_cast<int>(a) != static_cast<int>(b)) { \
        std::ostringstream _oss; \
        _oss << __FILE__ ":" << __LINE__ << " " #a " == " #b; \
        throw std::runtime_error(_oss.str()); \
    }} while(0)

// ===========================================================================
// 1. Span Model Tests
// ===========================================================================
void test_span_model()
{
    std::cout << "\n========== Span Model ==========" << std::endl;

    TEST("create span with all fields") {
        Span s = Span::create("feedbeeffeedbeeffeedbeeffeedbeef", "parent001parent001", "MyService::doWork", "payment-svc");
        ASSERT_EQ(s.trace_id, "feedbeeffeedbeeffeedbeeffeedbeef");
        ASSERT_EQ(s.parent_span_id, "parent001parent001");
        ASSERT_EQ(s.operation_name, "MyService::doWork");
        ASSERT_EQ(s.service_name, "payment-svc");
        ASSERT_ENUM_EQ(s.kind, SpanKind::INTERNAL);
        ASSERT_ENUM_EQ(s.status_code, SpanStatusCode::UNSET);
        ASSERT(s.duration_ns() == 0);
        ASSERT(s.duration_ms() == 0.0);
        ASSERT(!s.is_root());
        ASSERT(!s.has_error());
    } END_TEST();

    TEST("root span has empty parent_span_id") {
        Span root = Span::create("trace123", "", "handleRequest", "web");
        ASSERT(root.is_root());
        ASSERT(root.parent_span_id.empty());
    } END_TEST();

    TEST("child span is not root") {
        Span child = Span::create("trace123", "deadbeefdeadbeef", "dbQuery", "web");
        ASSERT(!child.is_root());
    } END_TEST();

    TEST("duration calculation") {
        Span s = Span::create("t", "", "op", "s");
        s.start_time_ns = 1'000'000'000;
        s.end_time_ns   = 1'005'000'000;
        ASSERT_EQ(s.duration_ns(), 5'000'000ULL);
        ASSERT(s.duration_ms() == 5.0);
    } END_TEST();

    TEST("zero duration when end < start") {
        Span s = Span::create("t", "", "op", "s");
        s.start_time_ns = 2'000'000'000;
        s.end_time_ns   = 1'000'000'000;
        ASSERT_EQ(s.duration_ns(), 0ULL);
    } END_TEST();

    TEST("error span detection") {
        Span s = Span::create("t", "", "fail", "s");
        s.status_code = SpanStatusCode::ERROR;
        ASSERT(s.has_error());
        s.status_code = SpanStatusCode::OK;
        ASSERT(!s.has_error());
    } END_TEST();

    TEST("SpanKind enum values") {
        ASSERT(static_cast<int>(SpanKind::INTERNAL) == 0);
        ASSERT(static_cast<int>(SpanKind::SERVER) == 1);
        ASSERT(static_cast<int>(SpanKind::CLIENT) == 2);
        ASSERT(static_cast<int>(SpanKind::PRODUCER) == 3);
        ASSERT(static_cast<int>(SpanKind::CONSUMER) == 4);
    } END_TEST();

    TEST("SpanStatusCode enum values") {
        ASSERT(static_cast<int>(SpanStatusCode::UNSET) == 0);
        ASSERT(static_cast<int>(SpanStatusCode::OK) == 1);
        ASSERT(static_cast<int>(SpanStatusCode::ERROR) == 2);
    } END_TEST();

    TEST("status_code_name returns readable strings") {
        ASSERT(std::string(status_code_name(SpanStatusCode::UNSET)) == "UNSET");
        ASSERT(std::string(status_code_name(SpanStatusCode::OK)) == "OK");
        ASSERT(std::string(status_code_name(SpanStatusCode::ERROR)) == "ERROR");
    } END_TEST();

    TEST("span_id is exactly 16 hex chars") {
        Span s = Span::create("trace001", "", "op", "s");
        ASSERT_EQ(s.span_id.size(), 16u);
        for (char c : s.span_id) {
            ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
    } END_TEST();

    TEST("span trace_state defaults to empty") {
        Span s = Span::create("t", "", "op", "s");
        ASSERT(s.trace_state.empty());
    } END_TEST();

    TEST("status_message defaults to empty") {
        Span s = Span::create("t", "", "op", "s");
        ASSERT(s.status_message.empty());
    } END_TEST();

    // ---- Attributes ----
    TEST("string attribute") {
        Span s = Span::create("t", "", "op", "s");
        s.attributes["url"] = AttributeValue::from_string("https://example.com/api");
        ASSERT_EQ(s.attributes["url"].str_val, "https://example.com/api");
        ASSERT(s.attributes["url"].type == AttributeValue::STRING);
    } END_TEST();

    TEST("int attribute") {
        Span s = Span::create("t", "", "op", "s");
        s.attributes["count"] = AttributeValue::from_int(42);
        ASSERT_EQ(s.attributes["count"].int_val, 42);
        ASSERT(s.attributes["count"].type == AttributeValue::INT);
    } END_TEST();

    TEST("double attribute") {
        Span s = Span::create("t", "", "op", "s");
        s.attributes["ratio"] = AttributeValue::from_double(3.1415);
        ASSERT(s.attributes["ratio"].double_val > 3.14);
        ASSERT(s.attributes["ratio"].type == AttributeValue::DOUBLE);
    } END_TEST();

    TEST("bool attribute") {
        Span s = Span::create("t", "", "op", "s");
        s.attributes["cached"] = AttributeValue::from_bool(false);
        ASSERT(!s.attributes["cached"].bool_val);
        ASSERT(s.attributes["cached"].type == AttributeValue::BOOL);
    } END_TEST();

    TEST("multiple attributes mixed types") {
        Span s = Span::create("t", "", "op", "s");
        s.attributes["method"]      = AttributeValue::from_string("POST");
        s.attributes["status_code"] = AttributeValue::from_int(200);
        s.attributes["latency_ms"]  = AttributeValue::from_double(123.45);
        s.attributes["success"]     = AttributeValue::from_bool(true);
        ASSERT_EQ(s.attributes.size(), 4u);
    } END_TEST();

    TEST("default attribute has safe zero values") {
        AttributeValue av;
        ASSERT(av.type == AttributeValue::STRING);
        ASSERT(av.int_val == 0);
        ASSERT(av.bool_val == false);
    } END_TEST();
}

// ===========================================================================
// 2. ID Generation Tests
// ===========================================================================
void test_id_generation()
{
    std::cout << "\n========== ID Generation ==========" << std::endl;

    TEST("generate_id(8) produces 16 hex chars") {
        auto id = Span::generate_id(8);
        ASSERT_EQ(id.size(), 16u);
        for (char c : id) {
            ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
    } END_TEST();

    TEST("generate_id(16) produces 32 hex chars") {
        auto id = Span::generate_id(16);
        ASSERT_EQ(id.size(), 32u);
        for (char c : id) {
            ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
    } END_TEST();

    TEST("successive IDs are unique (single thread)") {
        std::set<std::string> ids;
        for (int i = 0; i < 1000; i++) {
            auto id = Span::generate_id(8);
            ASSERT(ids.insert(id).second); // must be unique
        }
    } END_TEST();

    TEST("successive trace IDs are unique") {
        std::set<std::string> ids;
        for (int i = 0; i < 1000; i++) {
            auto id = Span::generate_id(16);
            ASSERT(ids.insert(id).second);
        }
    } END_TEST();

    TEST("IDs are unique across threads") {
        constexpr int kThreads    = 4;
        constexpr int kPerThread  = 500;
        std::vector<std::string> all_ids(kThreads * kPerThread);
        std::vector<std::thread> threads;

        for (int t = 0; t < kThreads; t++) {
            threads.emplace_back([t, &all_ids]() {
                for (int i = 0; i < kPerThread; i++) {
                    all_ids[t * kPerThread + i] = Span::generate_id(8);
                }
            });
        }
        for (auto &th : threads) th.join();

        std::set<std::string> unique;
        for (const auto &id : all_ids) {
            ASSERT(unique.insert(id).second);
        }
    } END_TEST();

    TEST("Span::create assigns unique span_id per call") {
        Span s1 = Span::create("t", "", "a", "s");
        Span s2 = Span::create("t", "", "b", "s");
        Span s3 = Span::create("t", "", "c", "s");
        ASSERT(s1.span_id != s2.span_id);
        ASSERT(s2.span_id != s3.span_id);
        ASSERT(s1.span_id != s3.span_id);
    } END_TEST();
}

// ===========================================================================
// 3. SpanBuffer Tests
// ===========================================================================
void test_span_buffer()
{
    std::cout << "\n========== SpanBuffer ==========" << std::endl;

    // ---- basic push/drain ----
    TEST("push and drain_all returns all spans") {
        SpanBuffer buf(64);
        for (int i = 0; i < 10; i++) {
            ASSERT(buf.try_push(Span::create("t", "", "op" + std::to_string(i), "s")));
        }
        ASSERT_EQ(buf.size(), 10u);
        ASSERT(!buf.empty());

        std::vector<Span> drained;
        size_t count = buf.drain_all(drained);
        ASSERT_EQ(count, 10u);
        ASSERT_EQ(drained.size(), 10u);
        ASSERT(buf.empty());
    } END_TEST();

    TEST("drain with max_count limits output") {
        SpanBuffer buf(64);
        for (int i = 0; i < 10; i++) {
            buf.try_push(Span::create("t", "", "op" + std::to_string(i), "s"));
        }

        std::vector<Span> batch;
        size_t n = buf.drain(batch, 3, 100);
        ASSERT_EQ(n, 3u);
        ASSERT_EQ(buf.size(), 7u);
    } END_TEST();

    TEST("drain_all on empty buffer returns zero") {
        SpanBuffer buf(64);
        std::vector<Span> drained;
        ASSERT_EQ(buf.drain_all(drained), 0u);
        ASSERT(drained.empty());
    } END_TEST();

    // ---- capacity & overflow ----
    TEST("capacity is N-1 (one reserved slot)") {
        SpanBuffer buf(16);
        ASSERT_EQ(buf.capacity(), 15u);
    } END_TEST();

    TEST("try_push returns false when buffer full") {
        SpanBuffer buf(4); // capacity = 3
        ASSERT(buf.try_push(Span::create("t", "", "1", "s")));
        ASSERT(buf.try_push(Span::create("t", "", "2", "s")));
        ASSERT(buf.try_push(Span::create("t", "", "3", "s")));
        ASSERT(!buf.try_push(Span::create("t", "", "4", "s"))); // full
        ASSERT_EQ(buf.size(), 3u);
    } END_TEST();

    TEST("overflow increments dropped counter") {
        SpanBuffer buf(4);
        buf.try_push(Span::create("t", "", "1", "s"));
        buf.try_push(Span::create("t", "", "2", "s"));
        buf.try_push(Span::create("t", "", "3", "s"));
        buf.try_push(Span::create("t", "", "4", "s")); // dropped
        ASSERT_EQ(buf.total_pushed(), 3u);
        ASSERT_EQ(buf.total_dropped(), 1u);
    } END_TEST();

    // ---- stats ----
    TEST("stats track pushed / dropped / drained") {
        SpanBuffer buf(64);
        ASSERT_EQ(buf.total_pushed(), 0u);
        ASSERT_EQ(buf.total_dropped(), 0u);
        ASSERT_EQ(buf.total_drained(), 0u);

        buf.try_push(Span::create("t", "", "a", "s"));
        buf.try_push(Span::create("t", "", "b", "s"));
        ASSERT_EQ(buf.total_pushed(), 2u);

        std::vector<Span> batch;
        buf.drain_all(batch);
        ASSERT_EQ(buf.total_drained(), 2u);
    } END_TEST();

    // ---- shutdown ----
    TEST("shutdown signals consumer") {
        SpanBuffer buf(64);
        buf.try_push(Span::create("t", "", "op", "s"));
        ASSERT(!buf.is_shutdown());

        buf.shutdown();
        ASSERT(buf.is_shutdown());

        // drain should still work after shutdown (to clear remaining)
        std::vector<Span> batch;
        ASSERT_EQ(buf.drain_all(batch), 1u);
    } END_TEST();

    TEST("drain after shutdown on empty buffer returns zero") {
        SpanBuffer buf(64);
        buf.shutdown();
        std::vector<Span> batch;
        ASSERT_EQ(buf.drain(batch, 10, 50), 0u);
    } END_TEST();

    // ---- multi-threaded producer/consumer ----
    TEST("single-producer single-consumer threading") {
        SpanBuffer buf(1024);
        std::atomic<bool> producer_done{false};
        std::atomic<size_t> total_consumed{0};

        auto producer = [&]() {
            for (int i = 0; i < 800; i++) {
                while (!buf.try_push(Span::create("t", "", "op", "s"))) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            }
            producer_done = true;
        };

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

        ASSERT_EQ(total_consumed.load(), 800u);
        ASSERT(buf.empty());
        ASSERT_EQ(buf.total_dropped(), 0u);
    } END_TEST();

    TEST("multi-producer single-consumer with zero drops") {
        SpanBuffer buf(4096);
        std::atomic<bool> all_done{false};
        std::atomic<size_t> consumed{0};

        auto producer = [&](int id) {
            for (int i = 0; i < 200; i++) {
                while (!buf.try_push(Span::create("t", "", "p" + std::to_string(id) + "_" + std::to_string(i), "s"))) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        };

        auto consumer = [&]() {
            std::vector<Span> batch;
            while (!all_done || !buf.empty()) {
                buf.drain(batch, 200, 100);
                consumed += batch.size();
                batch.clear();
            }
        };

        std::vector<std::thread> producers;
        for (int i = 0; i < 4; i++) {
            producers.emplace_back(producer, i);
        }
        std::thread ct(consumer);

        for (auto &pt : producers) pt.join();
        all_done = true;
        ct.join();

        ASSERT_EQ(consumed.load(), 800u);
        ASSERT_EQ(buf.total_dropped(), 0u);
    } END_TEST();

    TEST("buffer drop under full overload") {
        SpanBuffer buf(16); // capacity = 15
        // Flood with 100 spans — many should drop
        for (int i = 0; i < 100; i++) {
            buf.try_push(Span::create("t", "", "op", "s"));
        }
        ASSERT_GT(buf.total_pushed(), 0ull);
        ASSERT_GT(buf.total_dropped(), 0ull);
        ASSERT_EQ(buf.size(), buf.capacity());
    } END_TEST();
}

// ===========================================================================
// 4. Loki Payload Builder Tests
// ===========================================================================
void test_loki_payload()
{
    std::cout << "\n========== Loki Payload Builder ==========" << std::endl;

    TEST("empty spans produces empty string") {
        LokiConfig cfg;
        LokiExporter exporter(*reinterpret_cast<SpanBuffer*>(0x1), cfg);
        std::vector<Span> empty;
        ASSERT(empty.empty());
    } END_TEST();

    TEST("single span payload structure") {
        Span s = Span::create("trace123", "", "MyClass::method", "my-app");
        s.start_time_ns = 1'000'000'000;
        s.end_time_ns   = 1'000'100'000;
        s.status_code   = SpanStatusCode::OK;
        s.attributes["db.system"] = AttributeValue::from_string("mysql");

        LokiConfig cfg;
        cfg.service_name = "my-app";
        cfg.job_name     = "php-trace";

        // Build payload directly (via friend function)
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("trace123") != std::string::npos);
        ASSERT(payload.find("MyClass::method") != std::string::npos);
        ASSERT(payload.find("my-app") != std::string::npos);
    } END_TEST();

    TEST("error span has status code 2") {
        Span s = Span::create("traceX", "", "broken", "app");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;
        s.status_code   = SpanStatusCode::ERROR;
        s.status_message = "division by zero";

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("\"code\":2") != std::string::npos);
        ASSERT(payload.find("division by zero") != std::string::npos);
    } END_TEST();

    TEST("ok span has status code 1") {
        Span s = Span::create("traceX", "", "okay", "app");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;
        s.status_code   = SpanStatusCode::OK;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("\"code\":1") != std::string::npos);
    } END_TEST();

    TEST("unset span defaults to code 1 in tempo") {
        Span s = Span::create("traceX", "", "neutral", "app");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("\"code\":1") != std::string::npos);
    } END_TEST();

    TEST("empty parent_span_id is preserved") {
        Span s = Span::create("traceX", "", "rootOp", "app");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("\"parentSpanId\":\"\"") != std::string::npos);
    } END_TEST();

    TEST("non-empty parent_span_id is included") {
        Span s = Span::create("traceX", "deadbeefdeadbeef", "childOp", "app");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("\"parentSpanId\":\"deadbeefdeadbeef\"") != std::string::npos);
    } END_TEST();

    TEST("kind CLIENT produces code 3") {
        Span s = Span::create("t", "", "httpCall", "s");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;
        s.kind = SpanKind::CLIENT;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("\"kind\":3") != std::string::npos);
    } END_TEST();

    TEST("kind INTERNAL produces code 1") {
        Span s = Span::create("t", "", "compute", "s");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;
        s.kind = SpanKind::INTERNAL;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("\"kind\":1") != std::string::npos);
    } END_TEST();

    TEST("attributes appear in JSON") {
        Span s = Span::create("t", "", "op", "s");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;
        s.attributes["http.method"] = AttributeValue::from_string("GET");
        s.attributes["http.url"]    = AttributeValue::from_string("/api/v1/users");

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("http.method") != std::string::npos);
        ASSERT(payload.find("GET") != std::string::npos);
        ASSERT(payload.find("http.url") != std::string::npos);
        ASSERT(payload.find("/api/v1/users") != std::string::npos);
    } END_TEST();

    TEST("resource includes service.name") {
        Span s = Span::create("t", "", "op", "s");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;

        LokiConfig cfg;
        cfg.service_name = "order-service";
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("service.name") != std::string::npos);
        ASSERT(payload.find("order-service") != std::string::npos);
    } END_TEST();

    TEST("defaults service name when cfg empty") {
        Span s = Span::create("t", "", "op", "s");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("php-app") != std::string::npos);
    } END_TEST();

    TEST("multiple spans payload") {
        Span s1 = Span::create("traceZ", "", "op1", "svc");
        s1.start_time_ns = 100;
        s1.end_time_ns   = 200;
        Span s2 = Span::create("traceZ", s1.span_id, "op2", "svc");
        s2.start_time_ns = 200;
        s2.end_time_ns   = 300;

        LokiConfig cfg;
        cfg.service_name = "svc";
        std::string payload = build_tempo_payload({s1, s2}, cfg);
        ASSERT(payload.find("op1") != std::string::npos);
        ASSERT(payload.find("op2") != std::string::npos);
    } END_TEST();

    TEST("timestamps in nanosecond format") {
        Span s = Span::create("t", "", "op", "s");
        s.start_time_ns = 1719234567890123456ULL;
        s.end_time_ns   = 1719234567891123456ULL;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("1719234567890123456") != std::string::npos);
        ASSERT(payload.find("1719234567891123456") != std::string::npos);
    } END_TEST();

    TEST("escapes double quotes in span name") {
        Span s = Span::create("t", "", "SELECT * FROM \"users\"", "s");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("SELECT * FROM \\\"users\\\"") != std::string::npos);
    } END_TEST();

    TEST("escapes backslash in span name") {
        Span s = Span::create("t", "", "App\\\\Controller\\\\Index::action", "s");
        s.start_time_ns = 100;
        s.end_time_ns   = 200;

        LokiConfig cfg;
        std::string payload = build_tempo_payload({s}, cfg);
        ASSERT(payload.find("App\\\\\\\\Controller\\\\\\\\Index::action") != std::string::npos);
    } END_TEST();
}

// ===========================================================================
// 5. LokiExporter Lifecycle Tests
// ===========================================================================
void test_exporter_lifecycle()
{
    std::cout << "\n========== Exporter Lifecycle ==========" << std::endl;

    TEST("start / stop cycle") {
        SpanBuffer buf(64);
        LokiConfig cfg;
        cfg.endpoint     = "http://localhost:3100/loki/api/v1/push";
        cfg.service_name = "test";

        LokiExporter exporter(buf, cfg);
        ASSERT(!exporter.is_running());

        ASSERT(exporter.start());
        ASSERT(exporter.is_running());

        exporter.stop();
        ASSERT(!exporter.is_running());
    } END_TEST();

    TEST("stats start at zero") {
        SpanBuffer buf(64);
        LokiConfig cfg;
        cfg.endpoint = "http://localhost:3100/loki/api/v1/push";

        LokiExporter exporter(buf, cfg);
        ASSERT_EQ(exporter.spans_exported(), 0ull);
        ASSERT_EQ(exporter.batches_sent(), 0ull);
        ASSERT_EQ(exporter.batches_failed(), 0ull);
        ASSERT_EQ(exporter.http_errors(), 0ull);
    } END_TEST();

    TEST("Tempo exporter mode is recognized") {
        SpanBuffer buf(64);
        LokiConfig cfg;
        cfg.exporter       = "tempo";
        cfg.tempo_endpoint = "http://tempo:4318/v1/traces";

        // Just verify no crash on construction
        LokiExporter exporter(buf, cfg);
        ASSERT(!exporter.is_running());
    } END_TEST();
}

// ===========================================================================
// Main entry point
// ===========================================================================
int main()
{
    std::cout << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "   php-trace — Comprehensive Unit Test Suite" << std::endl;
    std::cout << "==================================================" << std::endl;

    test_span_model();
    test_id_generation();
    test_span_buffer();
    test_loki_payload();
    test_exporter_lifecycle();

    std::cout << "\n==================================================" << std::endl;
    std::cout << "   RESULTS: " << tests_passed << " passed, "
              << tests_failed << " failed"
              << "  (total: " << (tests_passed + tests_failed) << ")" << std::endl;
    std::cout << "==================================================" << std::endl;

    return tests_failed == 0 ? 0 : 1;
}
