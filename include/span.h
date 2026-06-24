/**
 * span.h - PHP Trace Span Data Model
 * 
 * Represents a single trace span — one instrumented operation
 * (function call, DB query, HTTP request, etc.).
 */

#ifndef PHP_TRACE_SPAN_H
#define PHP_TRACE_SPAN_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace php_trace {

// ---------------------------------------------------------------------------
// Status codes for spans (OpenTelemetry-compatible)
// ---------------------------------------------------------------------------
enum class SpanStatusCode : int {
    UNSET = 0,
    OK    = 1,
    ERROR = 2,
};

inline const char *status_code_name(SpanStatusCode code)
{
    switch (code) {
        case SpanStatusCode::OK: return "OK";
        case SpanStatusCode::ERROR: return "ERROR";
        case SpanStatusCode::UNSET:
        default: return "UNSET";
    }
}

// ---------------------------------------------------------------------------
// SpanKind
// ---------------------------------------------------------------------------
enum class SpanKind : int {
    INTERNAL = 0,
    SERVER   = 1,
    CLIENT   = 2,
    PRODUCER = 3,
    CONSUMER = 4,
};

// ---------------------------------------------------------------------------
// Attribute value — supports string, int, double, bool
// ---------------------------------------------------------------------------
struct AttributeValue {
    enum Type { STRING, INT, DOUBLE, BOOL };
    Type type;
    std::string str_val;
    int64_t int_val;
    double double_val;
    bool bool_val;

    AttributeValue() : type(STRING), int_val(0), double_val(0.0), bool_val(false) {}

    static AttributeValue from_string(const std::string &v) {
        AttributeValue av; av.type = STRING; av.str_val = v; return av;
    }
    static AttributeValue from_int(int64_t v) {
        AttributeValue av; av.type = INT; av.int_val = v; return av;
    }
    static AttributeValue from_double(double v) {
        AttributeValue av; av.type = DOUBLE; av.double_val = v; return av;
    }
    static AttributeValue from_bool(bool v) {
        AttributeValue av; av.type = BOOL; av.bool_val = v; return av;
    }
};

// ---------------------------------------------------------------------------
// Span — the core trace unit
// ---------------------------------------------------------------------------
struct Span {
    // ---- identity -----------------------------------
    std::string  trace_id;        // 32 hex chars
    std::string  span_id;         // 16 hex chars
    std::string  parent_span_id;  // 16 hex chars (empty = root)
    std::string  trace_state;     // W3C tracestate header (optional)

    // ---- operation ----------------------------------
    std::string  operation_name;  // e.g. "mysqli_query", "App\Controller\index"
    SpanKind     kind = SpanKind::INTERNAL;

    // ---- timing (nanoseconds since epoch) -----------
    uint64_t     start_time_ns = 0;
    uint64_t     end_time_ns   = 0;

    // ---- status -------------------------------------
    SpanStatusCode status_code = SpanStatusCode::UNSET;
    std::string    status_message;

    // ---- metadata -----------------------------------
    std::string                      service_name;
    std::unordered_map<std::string, AttributeValue> attributes;
    std::vector<Span>                events;       // span events (logs)
    std::vector<Span>                links;        // span links

    // ---- computed -----------------------------------
    uint64_t     depth = 0;         // nesting depth

    // ---- helpers ------------------------------------
    inline uint64_t duration_ns() const {
        return end_time_ns > start_time_ns ? end_time_ns - start_time_ns : 0;
    }

    inline double duration_ms() const {
        return duration_ns() / 1'000'000.0;
    }

    inline bool is_root() const {
        return parent_span_id.empty();
    }

    inline bool has_error() const {
        return status_code == SpanStatusCode::ERROR;
    }

    // ---- factory ------------------------------------
    static Span create(const std::string &trace_id,
                       const std::string &parent_span_id,
                       const std::string &operation_name,
                       const std::string &service_name);

    static std::string generate_id(int bytes);  // 8 or 16 bytes → hex
};

// ---------------------------------------------------------------------------
// Generate a random hex id of `bytes` length
// ---------------------------------------------------------------------------
inline std::string Span::generate_id(int bytes) {
    static thread_local bool seeded = false;
    static thread_local uint64_t counter = 0;
    
    if (!seeded) {
        auto now = std::chrono::high_resolution_clock::now()
                       .time_since_epoch()
                       .count();
        counter = static_cast<uint64_t>(now) ^ 
                  static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&counter));
        seeded = true;
    }
    
    ++counter;
    auto ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t hi = (static_cast<uint64_t>(ns) << 17) ^ counter;
    uint64_t lo = (ns * 6364136223846793005ULL) ^ (counter << 23);
    
    char buf[33];
    if (bytes == 8) {
        snprintf(buf, sizeof(buf), "%016llx", 
                 (unsigned long long)((hi ^ lo) & 0xFFFFFFFFFFFFFFFFULL));
    } else {
        snprintf(buf, sizeof(buf), "%016llx%016llx",
                 (unsigned long long)hi, (unsigned long long)lo);
    }
    return std::string(buf);
}

} // namespace php_trace

#endif // PHP_TRACE_SPAN_H
