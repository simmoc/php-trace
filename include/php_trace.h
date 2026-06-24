/**
 * php_trace.h - PHP Trace Extension Header
 *
 * A PHP extension that automatically instruments function/method calls
 * and exports trace spans to Grafana Loki.
 *
 * Features:
 *   - Hooks zend_execute_ex to capture function entry/exit timings
 *   - Thread-safe ring buffer for span collection
 *   - Background Loki exporter
 *   - Configurable sampling, filtering, and batching
 *   - W3C Trace Context propagation (incoming & outgoing)
 */

#ifndef PHP_TRACE_H
#define PHP_TRACE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_extensions.h"
#include "zend_execute.h"
#include "zend_API.h"

#include <string>
#include <vector>
#include <cstdint>

#include "span.h"
#include "span_buffer.h"
#include "loki_exporter.h"

// ---------------------------------------------------------------------------
// TraceContext — per-request TLS data (managed manually, stored as void*)
//
// We cannot put std::string or std::vector directly into ZEND globals
// because TSRM uses plain-old-data allocation with memset. Instead,
// we heap-allocate a TraceContext and store its pointer in globals.
// ---------------------------------------------------------------------------
struct TraceContext {
    std::string          trace_id;          // 32-char hex
    std::string          parent_span_id;    // 16-char hex (empty = root)
    uint64_t             depth = 0;
    bool                 in_trace = false;
    uint64_t             request_start_ns = 0;
    std::vector<uint64_t> span_stack;       // start timestamps for nesting
};

// ---------------------------------------------------------------------------
// Extension globals (per-process; char* pointers managed by PHP INI)
// ---------------------------------------------------------------------------
ZEND_BEGIN_MODULE_GLOBALS(php_trace)
    zend_bool      enabled;              // master on/off switch
    char          *service_name;         // service identifier
    char          *loki_endpoint;        // Loki push URL
    char          *loki_username;        // basic auth user
    char          *loki_password;        // basic auth password
    char          *loki_tenant_id;       // X-Scope-OrgID
    long           loki_batch_size;      // max spans per HTTP request
    long           loki_flush_interval;  // seconds between flushes
    double         sample_rate;          // 0.0 - 1.0
    long           max_spans;            // ring buffer capacity
    zend_bool      capture_args;         // capture function arguments
    zend_bool      capture_return;       // capture return value
    long           max_arg_length;       // truncate arg values > N chars
    zend_bool      trace_internal;       // trace built-in functions too
    zend_bool      trace_user;           // trace user functions
    char          *include_pattern;      // regex: only trace matching functions
    char          *exclude_pattern;      // regex: skip matching functions
    long           max_depth;            // max nesting depth to trace
    zend_bool      hook_installed;       // runtime: is execute_ex hook active?
ZEND_END_MODULE_GLOBALS(php_trace)

// Thread-local: opaque pointer to TraceContext (heap-allocated per request)
ZEND_BEGIN_MODULE_GLOBALS(php_trace_tls)
    void          *ctx;                  // TraceContext*
ZEND_END_MODULE_GLOBALS(php_trace_tls)

// ---------------------------------------------------------------------------
// Accessor macros
// ---------------------------------------------------------------------------
#ifdef ZTS
#define PHTRACE_G(v)    TSRMG(php_trace_globals_id, zend_php_trace_globals *, v)
#define PHTRACE_TLS(v)  TSRMG(php_trace_tls_globals_id, zend_php_trace_tls_globals *, v)
#else
#define PHTRACE_G(v)    (php_trace_globals.v)
#define PHTRACE_TLS(v)  (php_trace_tls_globals.v)
#endif

extern ZEND_DECLARE_MODULE_GLOBALS(php_trace)
extern ZEND_DECLARE_MODULE_GLOBALS(php_trace_tls)

// ---------------------------------------------------------------------------
// Inline helpers to access TraceContext safely
// ---------------------------------------------------------------------------
inline TraceContext* php_trace_get_ctx()
{
    auto *raw = static_cast<TraceContext*>(PHTRACE_TLS(ctx));
    if (raw == nullptr) {
        raw = new TraceContext();
        PHTRACE_TLS(ctx) = raw;
    }
    return raw;
}

inline void php_trace_free_ctx()
{
    auto *raw = static_cast<TraceContext*>(PHTRACE_TLS(ctx));
    if (raw) {
        delete raw;
        PHTRACE_TLS(ctx) = nullptr;
    }
}

inline bool php_trace_has_ctx()
{
    return PHTRACE_TLS(ctx) != nullptr;
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
extern php_trace::SpanBuffer    *g_span_buffer;
extern php_trace::LokiExporter  *g_loki_exporter;

// ---------------------------------------------------------------------------
// Hook management
// ---------------------------------------------------------------------------
void php_trace_install_hook();
void php_trace_remove_hook();

// ---------------------------------------------------------------------------
// Helper: should we trace this function?
// ---------------------------------------------------------------------------
bool php_trace_should_trace(const zend_function *func, const zend_string *fname);

// ---------------------------------------------------------------------------
// Helper: get current time in nanoseconds
// ---------------------------------------------------------------------------
uint64_t php_trace_now_ns();

#endif // PHP_TRACE_H
