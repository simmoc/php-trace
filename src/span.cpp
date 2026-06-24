/**
 * span.cpp - Span implementation
 */

#include "span.h"

namespace php_trace {

Span Span::create(const std::string &trace_id,
                  const std::string &parent_span_id,
                  const std::string &operation_name,
                  const std::string &service_name)
{
    Span s;
    s.trace_id       = trace_id;
    s.parent_span_id = parent_span_id;
    s.span_id        = generate_id(8);  // 16-char hex
    s.operation_name = operation_name;
    s.service_name   = service_name;
    s.kind           = SpanKind::INTERNAL;
    s.status_code    = SpanStatusCode::UNSET;

    auto now = std::chrono::high_resolution_clock::now()
                   .time_since_epoch()
                   .count();
    s.start_time_ns = static_cast<uint64_t>(now);

    return s;
}

} // namespace php_trace
