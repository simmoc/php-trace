/**
 * php_trace.cpp - PHP Trace Extension Core Implementation
 *
 * Hooks into PHP's zend_execute_ex to automatically instrument all
 * function/method calls. Captured spans are pushed to a ring buffer
 * and exported to Loki by a background thread.
 *
 * PHP 8.x compatible. Supports both NTS and ZTS builds.
 *
 * ============================================================================
 * HOW IT WORKS
 * ============================================================================
 *
 * 1. At MINIT, we save the original zend_execute_ex function pointer and
 *    replace it with our own wrapper.
 *
 * 2. Our wrapper is called for every user function invocation:
 *    - Before the original execute_ex: create a Span, record start time.
 *    - Call the original execute_ex.
 *    - After it returns: finalize the Span (duration, status), push to buffer.
 *
 * 3. If trace_internal is enabled, we also hook zend_execute_internal
 *    to trace built-in functions (mysqli_*, curl_*, etc.).
 *
 * 4. A background thread (LokiExporter) periodically drains the buffer
 *    and sends batches to Loki via HTTP.
 *
 * ============================================================================
 * THREAD SAFETY
 * ============================================================================
 *
 * - Span creation uses TraceContext (heap-allocated per request/TLS).
 * - SpanBuffer::try_push is mutex-protected for multi-producer safety.
 * - The LokiExporter runs in its own thread and is the sole consumer.
 * - TLS ctx pointer is zeroed by TSRM and lazily initialized.
 *
 * ============================================================================
 * W3C TRACE CONTEXT PROPAGATION
 * ============================================================================
 *
 * Incoming: If the request has a `traceparent` header, we extract the
 * trace_id and parent_span_id from it. This allows PHP traces to be
 * linked to upstream services.
 *
 * Outgoing: The trace_id is injected into the `traceresponse` header
 * via PHP's header() function or the $_SERVER superglobal for use by
 * downstream HTTP clients.
 */

#include "php_trace.h"

#include <regex>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>

// ===========================================================================
// Extension metadata
// ===========================================================================
#define PHP_TRACE_EXTNAME    "php_trace"
#define PHP_TRACE_VERSION    "1.0.0"
#define PHP_TRACE_AUTHOR     "php-trace team"
#define PHP_TRACE_URL        "https://github.com/simmoc/php-trace"

// ===========================================================================
// Globals
// ===========================================================================
ZEND_DECLARE_MODULE_GLOBALS(php_trace)
ZEND_DECLARE_MODULE_GLOBALS(php_trace_tls)

php_trace::SpanBuffer   *g_span_buffer   = nullptr;
php_trace::LokiExporter *g_loki_exporter = nullptr;

// Original zend_execute_ex — saved before we replace it
static void (*original_execute_ex)(zend_execute_data *execute_data) = nullptr;

// Original zend_execute_internal (optional)
static void (*original_execute_internal)(zend_execute_data *execute_data, zval *return_value) = nullptr;

// Mutex for hook installation (one-time)
static std::mutex g_hook_mutex;

// ===========================================================================
// Global constructor / destructor — NOT used; TraceContext is managed
// via RINIT/RSHUTDOWN (lazy allocation + explicit free). TSRM zeroes the
// void* ctx pointer by default, which is exactly what we want.
// ===========================================================================

// ===========================================================================
// INI entries
// ===========================================================================
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("php_trace.enabled",             "1",  PHP_INI_SYSTEM, OnUpdateBool,   enabled,              zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.service_name",        "php-app", PHP_INI_SYSTEM, OnUpdateString, service_name,     zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.loki.endpoint",       "http://localhost:3100/loki/api/v1/push", PHP_INI_SYSTEM, OnUpdateString, loki_endpoint, zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.loki.username",       "",   PHP_INI_SYSTEM, OnUpdateString, loki_username,        zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.loki.password",       "",   PHP_INI_SYSTEM, OnUpdateString, loki_password,        zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.loki.tenant_id",      "",   PHP_INI_SYSTEM, OnUpdateString, loki_tenant_id,       zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.loki.batch_size",     "512", PHP_INI_SYSTEM, OnUpdateLong,   loki_batch_size,      zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.loki.flush_interval", "5",   PHP_INI_SYSTEM, OnUpdateLong,   loki_flush_interval,  zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.sample_rate",         "1.0", PHP_INI_SYSTEM, OnUpdateReal,   sample_rate,          zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.max_spans",           "65536", PHP_INI_SYSTEM, OnUpdateLong, max_spans,           zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.capture_args",        "0",  PHP_INI_SYSTEM, OnUpdateBool,   capture_args,         zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.capture_return",      "0",  PHP_INI_SYSTEM, OnUpdateBool,   capture_return,       zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.max_arg_length",      "256", PHP_INI_SYSTEM, OnUpdateLong,   max_arg_length,       zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.trace_internal",      "1",  PHP_INI_SYSTEM, OnUpdateBool,   trace_internal,       zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.trace_user",          "0",  PHP_INI_SYSTEM, OnUpdateBool,   trace_user,           zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.include_pattern",     "^(mysqli_|mysql_|PDO::|PDOStatement::|curl_|fsockopen|stream_socket_client|file_get_contents|fopen|stream_socket_|socket_|redis_|ldap_|pg_|sqlite_)",   PHP_INI_SYSTEM, OnUpdateString, include_pattern,      zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.exclude_pattern",     "",   PHP_INI_SYSTEM, OnUpdateString, exclude_pattern,      zend_php_trace_globals, php_trace_globals)
    STD_PHP_INI_ENTRY("php_trace.max_depth",           "64", PHP_INI_SYSTEM, OnUpdateLong,   max_depth,            zend_php_trace_globals, php_trace_globals)
PHP_INI_END()

// ===========================================================================
// Resolve a config value from environment first, then INI, then fallback.
// ===========================================================================
static const char *php_trace_resolve_string_value(const char *ini_value, const char *env_name, const char *fallback)
{
    if (env_name) {
        const char *env_value = std::getenv(env_name);
        if (env_value && env_value[0] != '\0') {
            return env_value;
        }
    }

    if (ini_value && ini_value[0] != '\0') {
        return ini_value;
    }

    return fallback;
}

// ===========================================================================
// Load INI values into config struct
// ==========================================================================
static php_trace::LokiConfig php_trace_build_loki_config()
{
    php_trace::LokiConfig cfg;
    const char *default_endpoint = "http://localhost:3100/loki/api/v1/push";
    cfg.endpoint        = php_trace_resolve_string_value(PHTRACE_G(loki_endpoint), "PHP_TRACE_LOKI_ENDPOINT", default_endpoint);
    cfg.tempo_endpoint  = php_trace_resolve_string_value(nullptr, "PHP_TRACE_TEMPO_ENDPOINT", cfg.endpoint.c_str());
    cfg.exporter        = php_trace_resolve_string_value(nullptr, "PHP_TRACE_EXPORTER", "loki");
    cfg.username        = php_trace_resolve_string_value(PHTRACE_G(loki_username), "PHP_TRACE_LOKI_USERNAME", "");
    cfg.password        = php_trace_resolve_string_value(PHTRACE_G(loki_password), "PHP_TRACE_LOKI_PASSWORD", "");
    cfg.tenant_id       = php_trace_resolve_string_value(PHTRACE_G(loki_tenant_id), "PHP_TRACE_LOKI_TENANT_ID", "");
    cfg.service_name    = php_trace_resolve_string_value(PHTRACE_G(service_name), "PHP_TRACE_SERVICE_NAME", "php-app");
    cfg.batch_size      = static_cast<size_t>(PHTRACE_G(loki_batch_size));
    cfg.flush_interval_s= static_cast<int>(PHTRACE_G(loki_flush_interval));
    return cfg;
}

// ===========================================================================
// Time helper
// ===========================================================================
uint64_t php_trace_now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
}

// ===========================================================================
// Function name extraction
// ===========================================================================
static std::string php_trace_get_function_name(zend_execute_data *execute_data)
{
    if (!execute_data || !execute_data->func) return "(unknown)";

    zend_function *func = execute_data->func;

    if (func->common.function_name) {
        std::string name;

        // For methods: ClassName::methodName
#if PHP_VERSION_ID >= 80200
        if (zend_get_called_scope(execute_data) && func->common.scope) {
#else
        if (execute_data->called_scope && func->common.scope) {
#endif
            if (func->common.scope->name) {
                name = ZSTR_VAL(func->common.scope->name);
                name += "::";
            }
        }

        name += ZSTR_VAL(func->common.function_name);
        return name;
    }

    return "(anonymous)";
}

// ===========================================================================
// Sampling decision
// ===========================================================================
static bool php_trace_should_sample()
{
    double rate = PHTRACE_G(sample_rate);
    if (rate <= 0.0) return false;
    if (rate >= 1.0) return true;

    // Simple random sampling
    return (static_cast<double>(rand()) / RAND_MAX) < rate;
}

// ===========================================================================
// Filtering: should we trace this function?
// ===========================================================================
bool php_trace_should_trace(const zend_function *func, const zend_string *fname)
{
    if (!PHTRACE_G(enabled)) return false;

    std::string name;
    if (func && func->common.function_name) {
        name = ZSTR_VAL(func->common.function_name);
    } else if (fname) {
        name = ZSTR_VAL(fname);
    } else {
        return false;
    }

    // Depth check
    auto *ctx = php_trace_get_ctx();
    long max_depth = PHTRACE_G(max_depth);
    if (max_depth > 0 && ctx->depth >= static_cast<uint64_t>(max_depth)) {
        return false;
    }

    // Include pattern (if set, only trace matching)
    const char *include = PHTRACE_G(include_pattern);
    if (include && include[0] != '\0') {
        try {
            std::regex re(include);
            if (!std::regex_search(name, re)) return false;
        } catch (...) { /* invalid regex, skip filtering */ }
    }

    // Exclude pattern (if set, skip matching)
    const char *exclude = PHTRACE_G(exclude_pattern);
    if (exclude && exclude[0] != '\0') {
        try {
            std::regex re(exclude);
            if (std::regex_search(name, re)) return false;
        } catch (...) { /* invalid regex, skip filtering */ }
    }

    return true;
}

// ===========================================================================
// W3C Trace Context extraction from HTTP headers
// ===========================================================================
static void php_trace_extract_trace_context(TraceContext *ctx)
{
    zval *server = nullptr;
    zend_is_auto_global(ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_SERVER));

#if PHP_VERSION_ID >= 80100
    if (Z_TYPE(PG(http_globals)[TRACK_VARS_SERVER]) == IS_ARRAY) {
        server = &PG(http_globals)[TRACK_VARS_SERVER];
    }
#else
    if (PG(http_globals)[TRACK_VARS_SERVER]) {
        server = PG(http_globals)[TRACK_VARS_SERVER];
    }
#endif

    if (!server || Z_TYPE_P(server) != IS_ARRAY) return;

    zval *header = zend_hash_str_find(Z_ARRVAL_P(server), "HTTP_TRACEPARENT", sizeof("HTTP_TRACEPARENT") - 1);
    if (!header || Z_TYPE_P(header) != IS_STRING) return;

    std::string traceparent(Z_STRVAL_P(header), Z_STRLEN_P(header));

    // Parse: version-traceid-parentid-flags
    // Example: 00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01
    size_t first_dash  = traceparent.find('-');
    size_t second_dash = traceparent.find('-', first_dash + 1);
    size_t third_dash  = traceparent.find('-', second_dash + 1);

    if (first_dash == std::string::npos || second_dash == std::string::npos || third_dash == std::string::npos) {
        return; // malformed
    }

    ctx->trace_id = traceparent.substr(first_dash + 1, second_dash - first_dash - 1);
    ctx->parent_span_id = traceparent.substr(second_dash + 1, third_dash - second_dash - 1);
}

// ===========================================================================
// TraceContext initialization (called at RINIT to set up request tracing)
// ===========================================================================
static void php_trace_init_request()
{
    // Lazy-init TraceContext
    auto *ctx = php_trace_get_ctx();
    
    ctx->depth            = 0;
    ctx->in_trace         = false;
    ctx->request_start_ns = php_trace_now_ns();
    ctx->trace_id.clear();
    ctx->parent_span_id.clear();
    ctx->span_stack.clear();

    if (!PHTRACE_G(enabled)) return;
    if (!php_trace_should_sample()) return;

    // Extract incoming trace context
    php_trace_extract_trace_context(ctx);

    // If no incoming trace, generate new trace ID
    if (ctx->trace_id.empty()) {
        ctx->trace_id = php_trace::Span::generate_id(16); // 32 hex chars
    }

    ctx->in_trace = true;
}

// ===========================================================================
// Our replacement for zend_execute_ex
// ===========================================================================
static void php_trace_execute_ex(zend_execute_data *execute_data)
{
    if (!execute_data || !execute_data->func) {
        if (original_execute_ex) {
            original_execute_ex(execute_data);
        }
        return;
    }

    // Ensure TraceContext exists (lazy init)
    auto *ctx = php_trace_get_ctx();
    bool tracing = ctx->in_trace && PHTRACE_G(enabled);
    bool user_func = execute_data->func->type == ZEND_USER_FUNCTION;

    // Decide whether to trace this call
    bool trace_this = false;
    if (tracing) {
        if (user_func && PHTRACE_G(trace_user)) {
            trace_this = php_trace_should_trace(execute_data->func, nullptr);
        } else if (!user_func && PHTRACE_G(trace_internal)) {
            trace_this = php_trace_should_trace(execute_data->func, nullptr);
        }
    }

    std::string span_parent;
    uint64_t depth_before = 0;

    if (trace_this) {
        depth_before = ctx->depth;
        span_parent  = ctx->parent_span_id;
        ctx->depth++;
        ctx->span_stack.push_back(php_trace_now_ns());
    }

    // ==================== CALL ORIGINAL ====================
    if (original_execute_ex) {
        original_execute_ex(execute_data);
    }

    // ==================== FINALIZE SPAN ====================
    if (trace_this && g_span_buffer) {
        uint64_t end_ns = php_trace_now_ns();
        uint64_t start_ns = 0;

        if (!ctx->span_stack.empty()) {
            start_ns = ctx->span_stack.back();
            ctx->span_stack.pop_back();
        }

        std::string fname = php_trace_get_function_name(execute_data);
        std::string svc(PHTRACE_G(service_name) ? PHTRACE_G(service_name) : "php-app");

        php_trace::Span span = php_trace::Span::create(
            ctx->trace_id,
            span_parent,
            fname,
            svc
        );

        span.start_time_ns = start_ns > 0 ? start_ns : php_trace_now_ns();
        span.end_time_ns   = end_ns;
        span.depth         = depth_before;

        // Check for uncaught exceptions (PHP 8+)
        if (EG(exception)) {
            span.status_code = php_trace::SpanStatusCode::ERROR;
            zval *exception_zv = EG(exception);
            if (exception_zv && Z_TYPE_P(exception_zv) == IS_OBJECT) {
                zend_object *exception_obj = Z_OBJ_P(exception_zv);
                if (exception_obj && exception_obj->ce) {
                    zval *msg_zv = zend_read_property(exception_obj->ce, exception_obj, ZEND_STRL("message"), 0, NULL);
                    if (msg_zv && Z_TYPE_P(msg_zv) == IS_STRING) {
                        span.status_message = std::string(Z_STRVAL_P(msg_zv), Z_STRLEN_P(msg_zv));
                    } else if (msg_zv && Z_TYPE_P(msg_zv) != IS_UNDEF) {
                        zval tmp = *msg_zv;
                        zval_copy_ctor(&tmp);
                        convert_to_string(&tmp);
                        if (Z_TYPE(tmp) == IS_STRING) {
                            span.status_message = std::string(Z_STRVAL(tmp), Z_STRLEN(tmp));
                        }
                        zval_ptr_dtor(&tmp);
                    }
                }
            }
        } else {
            span.status_code = php_trace::SpanStatusCode::OK;
        }

        // Set this span as the parent for nested calls
        ctx->parent_span_id = span.span_id;
        ctx->depth = depth_before;

        // Push to buffer
        g_span_buffer->try_push(std::move(span));
    }
}

// ===========================================================================
// Replacement for zend_execute_internal (traces built-in functions)
// ===========================================================================
static void php_trace_execute_internal(zend_execute_data *execute_data, zval *return_value)
{
    if (!execute_data || !execute_data->func) {
        if (original_execute_internal) {
            original_execute_internal(execute_data, return_value);
        }
        return;
    }

    auto *ctx = php_trace_get_ctx();
    bool tracing = ctx->in_trace && PHTRACE_G(enabled) && PHTRACE_G(trace_internal);
    bool trace_this = tracing && php_trace_should_trace(execute_data->func, nullptr);

    std::string span_parent;
    uint64_t depth_before = 0;
    uint64_t start_ns = 0;

    if (trace_this) {
        depth_before = ctx->depth;
        span_parent  = ctx->parent_span_id;
        ctx->depth++;
        start_ns = php_trace_now_ns();
    }

    // Call original
    if (original_execute_internal) {
        original_execute_internal(execute_data, return_value);
    }

    // Finalize span
    if (trace_this && g_span_buffer) {
        uint64_t end_ns = php_trace_now_ns();

        std::string fname = php_trace_get_function_name(execute_data);
        std::string svc(PHTRACE_G(service_name) ? PHTRACE_G(service_name) : "php-app");

        php_trace::Span span = php_trace::Span::create(
            ctx->trace_id,
            span_parent,
            fname,
            svc
        );
        span.start_time_ns = start_ns;
        span.end_time_ns   = end_ns;
        span.depth         = depth_before;
        span.kind          = php_trace::SpanKind::CLIENT;

        span.status_code = EG(exception)
            ? php_trace::SpanStatusCode::ERROR
            : php_trace::SpanStatusCode::OK;

        ctx->parent_span_id = span.span_id;
        ctx->depth = depth_before;

        g_span_buffer->try_push(std::move(span));
    }
}

// ===========================================================================
// Hook management
// ===========================================================================
void php_trace_install_hook()
{
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (PHTRACE_G(hook_installed)) return;

    original_execute_ex = zend_execute_ex;
    zend_execute_ex = php_trace_execute_ex;

    if (PHTRACE_G(trace_internal)) {
        original_execute_internal = zend_execute_internal;
        zend_execute_internal = php_trace_execute_internal;
    }

    PHTRACE_G(hook_installed) = 1;
}

void php_trace_remove_hook()
{
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (!PHTRACE_G(hook_installed)) return;

    zend_execute_ex = original_execute_ex;
    original_execute_ex = nullptr;

    if (original_execute_internal) {
        zend_execute_internal = original_execute_internal;
        original_execute_internal = nullptr;
    }

    PHTRACE_G(hook_installed) = 0;
}

// ===========================================================================
// PHP module lifecycle callbacks
// ===========================================================================

PHP_MINIT_FUNCTION(php_trace)
{
    REGISTER_INI_ENTRIES();

    long max_spans = PHTRACE_G(max_spans);
    if (max_spans <= 0) max_spans = 65536;
    g_span_buffer = new php_trace::SpanBuffer(static_cast<size_t>(max_spans));

    auto cfg = php_trace_build_loki_config();
    g_loki_exporter = new php_trace::LokiExporter(*g_span_buffer, cfg);

    if (PHTRACE_G(enabled)) {
        php_trace_install_hook();
        g_loki_exporter->start();
    }

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(php_trace)
{
    if (g_loki_exporter) {
        g_loki_exporter->stop();
        delete g_loki_exporter;
        g_loki_exporter = nullptr;
    }

    php_trace_remove_hook();

    if (g_span_buffer) {
        delete g_span_buffer;
        g_span_buffer = nullptr;
    }

    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(php_trace)
{
    php_trace_init_request();

    if (PHTRACE_G(enabled) && !PHTRACE_G(hook_installed)) {
        php_trace_install_hook();
    }

    if (g_loki_exporter && !g_loki_exporter->is_running()) {
        g_loki_exporter->start();
    }

    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(php_trace)
{
    // Free the per-request TraceContext
    php_trace_free_ctx();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(php_trace)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "php_trace support", "enabled");
    php_info_print_table_row(2, "Version", PHP_TRACE_VERSION);
    php_info_print_table_row(2, "Enabled", PHTRACE_G(enabled) ? "true" : "false");
    php_info_print_table_row(2, "Loki Endpoint", PHTRACE_G(loki_endpoint));
    php_info_print_table_row(2, "Service Name", PHTRACE_G(service_name));
    
    char rate_buf[32];
    snprintf(rate_buf, sizeof(rate_buf), "%.2f", PHTRACE_G(sample_rate));
    php_info_print_table_row(2, "Sample Rate", rate_buf);
    php_info_print_table_row(2, "Hook Installed", PHTRACE_G(hook_installed) ? "yes" : "no");

    if (g_span_buffer) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%zu / %zu (dropped: %llu)",
                 g_span_buffer->size(),
                 g_span_buffer->capacity(),
                 (unsigned long long)g_span_buffer->total_dropped());
        php_info_print_table_row(2, "Buffer", buf);
    }

    if (g_loki_exporter) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "exported: %llu, sent: %llu, failed: %llu, http_err: %llu",
                 (unsigned long long)g_loki_exporter->spans_exported(),
                 (unsigned long long)g_loki_exporter->batches_sent(),
                 (unsigned long long)g_loki_exporter->batches_failed(),
                 (unsigned long long)g_loki_exporter->http_errors());
        php_info_print_table_row(2, "Loki Stats", buf);
    }

    php_info_print_table_end();
}

// ===========================================================================
// PHP-callable functions
// ===========================================================================

PHP_FUNCTION(php_trace_status)
{
    array_init(return_value);

    add_assoc_bool(return_value,  "enabled",        static_cast<zend_bool>(PHTRACE_G(enabled)));
    add_assoc_bool(return_value,  "hook_installed", static_cast<zend_bool>(PHTRACE_G(hook_installed)));
    add_assoc_string(return_value, "service_name",  PHTRACE_G(service_name) ? PHTRACE_G(service_name) : "");
    add_assoc_double(return_value, "sample_rate",   PHTRACE_G(sample_rate));

    if (g_span_buffer) {
        add_assoc_long(return_value, "buffer_size",     static_cast<zend_long>(g_span_buffer->size()));
        add_assoc_long(return_value, "buffer_capacity", static_cast<zend_long>(g_span_buffer->capacity()));
        add_assoc_long(return_value, "total_pushed",    static_cast<zend_long>(g_span_buffer->total_pushed()));
        add_assoc_long(return_value, "total_dropped",   static_cast<zend_long>(g_span_buffer->total_dropped()));
        add_assoc_long(return_value, "total_drained",   static_cast<zend_long>(g_span_buffer->total_drained()));
    }

    if (g_loki_exporter) {
        add_assoc_bool(return_value,  "exporter_running", static_cast<zend_bool>(g_loki_exporter->is_running()));
        add_assoc_long(return_value,  "spans_exported",   static_cast<zend_long>(g_loki_exporter->spans_exported()));
        add_assoc_long(return_value,  "batches_sent",     static_cast<zend_long>(g_loki_exporter->batches_sent()));
        add_assoc_long(return_value,  "batches_failed",   static_cast<zend_long>(g_loki_exporter->batches_failed()));
    }

    // Per-request trace context
    if (php_trace_has_ctx()) {
        auto *ctx = php_trace_get_ctx();
        add_assoc_bool(return_value,   "in_trace",       static_cast<zend_bool>(ctx->in_trace));
        add_assoc_string(return_value, "trace_id",       const_cast<char *>(ctx->trace_id.c_str()));
        add_assoc_long(return_value,   "current_depth",  static_cast<zend_long>(ctx->depth));
    } else {
        add_assoc_bool(return_value,  "in_trace",       false);
        add_assoc_string(return_value, "trace_id",      "");
        add_assoc_long(return_value,  "current_depth",  0);
    }
}

PHP_FUNCTION(php_trace_create_span)
{
    char *name;
    size_t name_len;
    HashTable *attrs = nullptr;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(attrs)
    ZEND_PARSE_PARAMETERS_END();

    if (!php_trace_has_ctx()) {
        RETURN_NULL();
    }
    auto *ctx = php_trace_get_ctx();
    if (!ctx->in_trace) {
        RETURN_NULL();
    }

    std::string op_name(name, name_len);
    std::string svc(PHTRACE_G(service_name) ? PHTRACE_G(service_name) : "php-app");

    php_trace::Span span = php_trace::Span::create(
        ctx->trace_id,
        ctx->parent_span_id,
        op_name,
        svc
    );

    if (attrs) {
        zend_string *key;
        zval *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(attrs, key, val) {
            if (!key) continue;
            std::string k(ZSTR_VAL(key));
            if (Z_TYPE_P(val) == IS_STRING) {
                span.attributes[k] = php_trace::AttributeValue::from_string(
                    std::string(Z_STRVAL_P(val), Z_STRLEN_P(val)));
            } else if (Z_TYPE_P(val) == IS_LONG) {
                span.attributes[k] = php_trace::AttributeValue::from_int(Z_LVAL_P(val));
            } else if (Z_TYPE_P(val) == IS_DOUBLE) {
                span.attributes[k] = php_trace::AttributeValue::from_double(Z_DVAL_P(val));
            } else if (Z_TYPE_P(val) == IS_TRUE || Z_TYPE_P(val) == IS_FALSE) {
                span.attributes[k] = php_trace::AttributeValue::from_bool(Z_TYPE_P(val) == IS_TRUE);
            }
        } ZEND_HASH_FOREACH_END();
    }

    span.start_time_ns = php_trace_now_ns();
    span.end_time_ns   = span.start_time_ns;
    span.status_code   = php_trace::SpanStatusCode::OK;

    // NOTE: In a full implementation, we'd store this span in a map keyed by
    // span_id so php_trace_finalize_span can find and complete it. For the
    // prototype, we return the id — the caller should call finalize quickly.
    // A production version should add a span registry (e.g. std::unordered_map).
    RETURN_STRINGL(span.span_id.c_str(), span.span_id.size());
}

PHP_FUNCTION(php_trace_finalize_span)
{
    char *span_id;
    size_t span_id_len;
    zend_long status = 1; // default: OK

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(span_id, span_id_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();

    if (!g_span_buffer || !php_trace_has_ctx()) {
        RETURN_FALSE;
    }

    auto *ctx = php_trace_get_ctx();
    std::string sid(span_id, span_id_len);
    std::string svc(PHTRACE_G(service_name) ? PHTRACE_G(service_name) : "php-app");

    php_trace::Span span = php_trace::Span::create(
        ctx->trace_id,
        ctx->parent_span_id,
        "manual",
        svc
    );
    span.span_id     = sid;
    span.end_time_ns = php_trace_now_ns();
    span.status_code = static_cast<php_trace::SpanStatusCode>(
        (status >= 1 && status <= 2) ? status : 1
    );

    g_span_buffer->try_push(std::move(span));
    RETURN_TRUE;
}

// ===========================================================================
// Function table
// ===========================================================================
ZEND_BEGIN_ARG_INFO(arginfo_php_trace_status, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_php_trace_create_span, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, attributes, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_php_trace_finalize_span, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, span_id, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 1)
ZEND_END_ARG_INFO()

static const zend_function_entry php_trace_functions[] = {
    PHP_FE(php_trace_status,         arginfo_php_trace_status)
    PHP_FE(php_trace_create_span,    arginfo_php_trace_create_span)
    PHP_FE(php_trace_finalize_span,  arginfo_php_trace_finalize_span)
    PHP_FE_END
};

// ===========================================================================
// Module definition with TLS CTOR/DTOR
// ===========================================================================
zend_module_entry php_trace_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_TRACE_EXTNAME,
    php_trace_functions,
    PHP_MINIT(php_trace),
    PHP_MSHUTDOWN(php_trace),
    PHP_RINIT(php_trace),
    PHP_RSHUTDOWN(php_trace),
    PHP_MINFO(php_trace),
    PHP_TRACE_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PHP_TRACE
ZEND_GET_MODULE(php_trace)
#endif
