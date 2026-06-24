/**
 * loki_exporter.cpp - Loki HTTP exporter implementation
 *
 * Converts Span objects to Loki-compatible JSON log entries and
 * sends them via HTTP POST to /loki/api/v1/push.
 *
 * Loki log entry format (each span = one log line):
 *   {
 *     "streams": [{
 *       "stream": {
 *         "job": "php-trace",
 *         "service": "my-app",
 *         "operation": "App\\Service\\getUser"
 *       },
 *       "values": [
 *         ["1623000000000000000", "{\"trace_id\":\"...\",\"duration_ms\":1.2,...}"]
 *       ]
 *     }]
 *   }
 */

#include "loki_exporter.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <stdexcept>

namespace php_trace {

// ===========================================================================
// Helpers
// ===========================================================================

/** Minimal JSON string escaping */
static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/** Format attribute value as JSON */
static void attr_to_json(std::ostringstream &os, const AttributeValue &av)
{
    switch (av.type) {
        case AttributeValue::STRING: os << "\"" << json_escape(av.str_val) << "\""; break;
        case AttributeValue::INT:    os << av.int_val; break;
        case AttributeValue::DOUBLE: os << av.double_val; break;
        case AttributeValue::BOOL:   os << (av.bool_val ? "true" : "false"); break;
    }
}

/** Low-level libcurl write callback */
static size_t curl_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *response = static_cast<std::string *>(userdata);
    size_t total = size * nmemb;
    response->append(static_cast<const char *>(ptr), total);
    return total;
}

// ===========================================================================
// Constructor / Destructor
// ===========================================================================

LokiExporter::LokiExporter(SpanBuffer &buffer, const LokiConfig &config)
    : buffer_(buffer), config_(config)
{
    curl_global_init(CURL_GLOBAL_ALL);
}

LokiExporter::~LokiExporter()
{
    stop();
    if (curl_headers_) {
        curl_slist_free_all(curl_headers_);
        curl_headers_ = nullptr;
    }
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
    curl_global_cleanup();
}

// ===========================================================================
// Start / Stop
// ===========================================================================

bool LokiExporter::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return true; // already running
    }

    setup_curl();

    stop_requested_.store(false, std::memory_order_release);

    try {
        worker_thread_ = std::thread(&LokiExporter::worker_loop, this);
        running_.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

void LokiExporter::stop()
{
    if (!running_.load(std::memory_order_acquire)) return;

    stop_requested_.store(true, std::memory_order_release);
    buffer_.shutdown();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    running_.store(false, std::memory_order_release);
}

bool LokiExporter::is_running() const
{
    return running_.load(std::memory_order_acquire);
}

uint64_t LokiExporter::spans_exported() const { return spans_exported_.load(std::memory_order_relaxed); }
uint64_t LokiExporter::batches_sent()   const { return batches_sent_.load(std::memory_order_relaxed); }
uint64_t LokiExporter::batches_failed() const { return batches_failed_.load(std::memory_order_relaxed); }
uint64_t LokiExporter::http_errors()    const { return http_errors_.load(std::memory_order_relaxed); }

// ===========================================================================
// Worker loop
// ===========================================================================

void LokiExporter::worker_loop()
{
    std::vector<Span> batch;
    batch.reserve(config_.batch_size);

    while (!stop_requested_.load(std::memory_order_acquire)) {
        // Drain spans from buffer (blocking with timeout)
        size_t drained = buffer_.drain(batch, config_.batch_size, config_.flush_interval_s * 1000);

        if (drained == 0) {
            // Timeout: no data available, loop back
            if (stop_requested_.load(std::memory_order_acquire)) break;
            continue;
        }

        // Build and send payload
        std::string payload = build_payload(batch);
        if (!payload.empty()) {
            bool ok = false;
            for (int retry = 0; retry <= config_.max_retries; ++retry) {
                ok = send_to_loki(payload);
                if (ok) break;
                if (retry < config_.max_retries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_delay_ms));
                }
            }
            
            if (ok) {
                spans_exported_.fetch_add(batch.size(), std::memory_order_relaxed);
                batches_sent_.fetch_add(1, std::memory_order_relaxed);
            } else {
                batches_failed_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        batch.clear();
    }

    // Final drain on shutdown
    std::vector<Span> remaining;
    buffer_.drain_all(remaining);
    if (!remaining.empty()) {
        std::string payload = build_payload(remaining);
        if (!payload.empty()) {
            for (int retry = 0; retry <= config_.max_retries; ++retry) {
                if (send_to_loki(payload)) break;
                if (retry < config_.max_retries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_delay_ms));
                }
            }
        }
        spans_exported_.fetch_add(remaining.size(), std::memory_order_relaxed);
    }
}

// ===========================================================================
// Build Loki payload
// ===========================================================================

std::string build_tempo_payload(const std::vector<Span> &spans, const LokiConfig &config)
{
    if (spans.empty()) return "";

    std::ostringstream os;
    os << "{\"resourceSpans\":[{";
    os << "\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":\"" << json_escape(config.service_name.empty() ? "php-app" : config.service_name) << "\"}}]}",
    os << ",\"scopeSpans\":[{";
    os << "\"scope\":{\"name\":\"php-trace\"},";
    os << "\"spans\":[";

    bool first_span = true;
    for (const auto &span : spans) {
        if (!first_span) os << ",";
        first_span = false;

        os << "{";
        os << "\"traceId\":\"" << json_escape(span.trace_id) << "\"";
        os << ",\"spanId\":\"" << json_escape(span.span_id.empty() ? span.trace_id.substr(0, 16) : span.span_id) << "\"";
        os << ",\"parentSpanId\":\"" << json_escape(span.parent_span_id) << "\"";
        os << ",\"name\":\"" << json_escape(span.operation_name) << "\"";
        os << ",\"kind\":" << (span.kind == SpanKind::CLIENT ? 3 : 1);
        os << ",\"startTimeUnixNano\":" << span.start_time_ns;
        os << ",\"endTimeUnixNano\":" << span.end_time_ns;
        os << ",\"status\":{\"code\":" << (span.status_code == SpanStatusCode::ERROR ? 2 : 1);
        if (!span.status_message.empty()) {
            os << ",\"message\":\"" << json_escape(span.status_message) << "\"";
        }
        os << "}";

        if (!span.attributes.empty()) {
            os << ",\"attributes\":[";
            bool first_attr = true;
            for (const auto &kv : span.attributes) {
                if (!first_attr) os << ",";
                first_attr = false;
                os << "{\"key\":\"" << json_escape(kv.first) << "\",\"value\":{";
                switch (kv.second.type) {
                    case AttributeValue::STRING:
                        os << "\"stringValue\":\"" << json_escape(kv.second.str_val) << "\"";
                        break;
                    case AttributeValue::INT:
                        os << "\"intValue\":\"" << kv.second.int_val << "\"";
                        break;
                    case AttributeValue::DOUBLE:
                        os << "\"doubleValue\":\"" << kv.second.double_val << "\"";
                        break;
                    case AttributeValue::BOOL:
                        os << "\"boolValue\":" << (kv.second.bool_val ? "true" : "false");
                        break;
                }
                os << "}}";
            }
            os << "]";
        }

        os << "}";
    }

    os << "]}]}]}";
    return os.str();
}

std::string LokiExporter::build_payload(const std::vector<Span> &spans)
{
    if (spans.empty()) return "";
    if (config_.exporter == "tempo") {
        return build_tempo_payload(spans, config_);
    }

    std::ostringstream os;
    os << "{\"streams\":[";

    bool first_stream = true;

    // Group spans by (job, service, operation) to create streams
    // For simplicity, we create one stream per span — Loki handles
    // high cardinality well with its index design.
    for (const auto &span : spans) {
        if (!first_stream) os << ",";
        first_stream = false;

        // Stream labels
        os << "{\"stream\":{";
        os << "\"job\":\"" << json_escape(config_.job_name) << "\"";
        os << ",\"service\":\"" << json_escape(span.service_name.empty() ? config_.service_name : span.service_name) << "\"";
        os << ",\"operation\":\"" << json_escape(span.operation_name) << "\"";
        os << ",\"kind\":\"" << static_cast<int>(span.kind) << "\"";

        // Extra labels from config
        for (const auto &kv : config_.extra_labels) {
            os << ",\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
        }

        os << "},\"values\":[[";
        // Loki expects nanosecond epoch timestamp as string
        os << "\"" << span.start_time_ns << "\"";
        os << ",\"";

        // The log line: JSON-serialized span details
        std::ostringstream line;
        line << "{";
        line << "\"trace_id\":\"" << json_escape(span.trace_id) << "\"";
        line << ",\"span_id\":\"" << json_escape(span.span_id) << "\"";
        line << ",\"parent_span_id\":\"" << json_escape(span.parent_span_id) << "\"";
        line << ",\"operation_name\":\"" << json_escape(span.operation_name) << "\"";
        line << ",\"service_name\":\"" << json_escape(span.service_name) << "\"";
        line << ",\"kind\":" << static_cast<int>(span.kind);
        line << ",\"start_time_ns\":" << span.start_time_ns;
        line << ",\"end_time_ns\":" << span.end_time_ns;
        line << ",\"duration_ns\":" << span.duration_ns();
        line << ",\"duration_ms\":" << span.duration_ms();
        line << ",\"status\":\"" << json_escape(status_code_name(span.status_code)) << "\"";
        if (!span.status_message.empty()) {
            line << ",\"status_message\":\"" << json_escape(span.status_message) << "\"";
        }
        line << ",\"depth\":" << span.depth;
        line << ",\"is_root\":" << (span.is_root() ? "true" : "false");

        // Attributes
        if (!span.attributes.empty()) {
            line << ",\"attributes\":{";
            bool first_attr = true;
            for (const auto &kv : span.attributes) {
                if (!first_attr) line << ",";
                first_attr = false;
                line << "\"" << json_escape(kv.first) << "\":";
                attr_to_json(line, kv.second);
            }
            line << "}";
        }

        line << "}";

        os << json_escape(line.str());
        os << "\"]]}";
    }

    os << "]}";
    return os.str();
}

// ===========================================================================
// HTTP transport
// ===========================================================================

void LokiExporter::setup_curl()
{
    if (curl_) {
        curl_easy_cleanup(curl_);
    }

    curl_ = curl_easy_init();
    if (!curl_) return;

    const std::string endpoint = (config_.exporter == "tempo" && !config_.tempo_endpoint.empty())
        ? config_.tempo_endpoint
        : config_.endpoint;
    curl_easy_setopt(curl_, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, static_cast<long>(config_.connect_timeout_s));
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, static_cast<long>(config_.request_timeout_s));
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, "php-trace-loki-exporter/1.0");
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, 30L);

    // Free previous header list
    if (curl_headers_) {
        curl_slist_free_all(curl_headers_);
        curl_headers_ = nullptr;
    }

    // Content-Type
    curl_headers_ = curl_slist_append(curl_headers_, "Content-Type: application/json");

    // Tenant ID (multi-tenancy) — must build persistent string because
    // curl_slist_append copies it, but the curl_headers_ list itself
    // must outlive the CURLOPT_HTTPHEADER assignment.
    if (!config_.tenant_id.empty()) {
        // Store tenant header in a member to keep it alive
        tenant_header_str_ = "X-Scope-OrgID: " + config_.tenant_id;
        curl_headers_ = curl_slist_append(curl_headers_, tenant_header_str_.c_str());
    }

    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, curl_headers_);

    // Basic auth
    if (!config_.username.empty()) {
        curl_easy_setopt(curl_, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl_, CURLOPT_USERNAME, config_.username.c_str());
        curl_easy_setopt(curl_, CURLOPT_PASSWORD, config_.password.c_str());
    }
}

bool LokiExporter::send_to_loki(const std::string &payload)
{
    if (!curl_) {
        setup_curl();
        if (!curl_) return false;
    }

    std::string response_body;
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);

    CURLcode res = curl_easy_perform(curl_);

    if (res != CURLE_OK) {
        http_errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code >= 200 && http_code < 300) {
        return true;
    }

    // Log the failure (in a real implementation, use php_error or a logger)
    http_errors_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

} // namespace php_trace
