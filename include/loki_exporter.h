/**
 * loki_exporter.h - Loki Log Exporter for PHP Trace Spans
 *
 * Formats spans as JSON log lines compatible with Grafana Loki's push API:
 *   POST /loki/api/v1/push
 *
 * Each span becomes a log stream entry:
 *   {
 *     "streams": [{
 *       "stream": { "job": "php-trace", "service": "my-app" },
 *       "values": [ [ "<unix_nano>", "<json_line>" ], ... ]
 *     }]
 *   }
 *
 * The exporter runs a background thread that periodically drains the
 * span buffer and sends batches to Loki via HTTP.
 */

#ifndef PHP_TRACE_LOKI_EXPORTER_H
#define PHP_TRACE_LOKI_EXPORTER_H

#include "span_buffer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <curl/curl.h>

namespace php_trace {

// ---------------------------------------------------------------------------
// LokiExporter configuration
// ---------------------------------------------------------------------------
struct LokiConfig {
    std::string endpoint;        // Loki push URL, e.g. "http://localhost:3100/loki/api/v1/push"
    std::string tempo_endpoint;  // Tempo OTLP/HTTP endpoint, e.g. "http://tempo:3200/v1/traces"
    std::string exporter = "loki"; // "loki" or "tempo"
    std::string username;        // basic auth (optional)
    std::string password;        // basic auth (optional)
    std::string tenant_id;       // X-Scope-OrgID header (optional)
    std::string job_name = "php-trace";
    std::string service_name = "php-app";
    
    // Batching
    size_t batch_size       = 512;  // max spans per HTTP request
    int    flush_interval_s = 5;    // flush at least every N seconds
    int    connect_timeout_s = 5;
    int    request_timeout_s = 30;
    int    max_retries     = 3;
    int    retry_delay_ms  = 1000;

    // Extra labels attached to every stream
    std::vector<std::pair<std::string, std::string>> extra_labels;
};

// ---------------------------------------------------------------------------
// LokiExporter — background thread that drains SpanBuffer → Loki
// ---------------------------------------------------------------------------
std::string build_tempo_payload(const std::vector<Span> &spans, const LokiConfig &config);

class LokiExporter {
public:
    explicit LokiExporter(SpanBuffer &buffer, const LokiConfig &config);
    ~LokiExporter();

    // Start the background flush thread.
    // Returns false if the thread could not be created.
    bool start();

    // Signal the background thread to stop and join it.
    void stop();

    // Whether the exporter is currently running.
    bool is_running() const;

    // Stats
    uint64_t spans_exported() const;
    uint64_t batches_sent() const;
    uint64_t batches_failed() const;
    uint64_t http_errors() const;

private:
    void worker_loop();

    /**
     * Build the Loki push JSON payload for a batch of spans.
     * Returns the JSON string, or empty on failure.
     */
    std::string build_payload(const std::vector<Span> &spans);

    /**
     * Send a payload to Loki. Returns true on success (2xx).
     */
    bool send_to_loki(const std::string &payload);

    /**
     * Set up CURL handle with auth, headers, etc.
     */
    void setup_curl();

    SpanBuffer  &buffer_;
    LokiConfig   config_;

    std::thread   worker_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Stats
    std::atomic<uint64_t> spans_exported_{0};
    std::atomic<uint64_t> batches_sent_{0};
    std::atomic<uint64_t> batches_failed_{0};
    std::atomic<uint64_t> http_errors_{0};

    CURL          *curl_ = nullptr;
    curl_slist    *curl_headers_ = nullptr;  // owned; freed on teardown
    std::string    tenant_header_str_;        // persists for curl header lifetime
};

} // namespace php_trace

#endif // PHP_TRACE_LOKI_EXPORTER_H
