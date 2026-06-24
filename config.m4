dnl config.m4 — PHP Trace Extension (Unix/Linux)
dnl
dnl Build with: phpize && ./configure --enable-php-trace && make && make install
dnl

PHP_ARG_ENABLE([php-trace],
    [whether to enable PHP Trace support],
    [AS_HELP_STRING([--enable-php-trace],
        [Enable PHP Trace extension with Loki support])],
    [no])

if test "$PHP_PHP_TRACE" != "no"; then
    dnl ------------------------------------------------------------------
    dnl Check for libcurl (required for Loki HTTP export)
    dnl ------------------------------------------------------------------
    PHP_CHECK_LIBRARY(curl, curl_easy_init,
    [
        PHP_ADD_LIBRARY(curl, , PHP_TRACE_SHARED_LIBADD)
    ], [
        AC_MSG_ERROR([libcurl is required for php_trace Loki export. Install libcurl-dev.])
    ])

    dnl ------------------------------------------------------------------
    dnl Check for C++17 support
    dnl ------------------------------------------------------------------
    PHP_REQUIRE_CXX()
    PHP_ADD_LIBRARY(stdc++, 1, PHP_TRACE_SHARED_LIBADD)

    CXXFLAGS="-std=c++17 -Wall -Wextra -O2 -g $CXXFLAGS"
    
    dnl ------------------------------------------------------------------
    dnl PHP 8.0+ required (for zend_execute_ex hook compatibility)
    dnl ------------------------------------------------------------------
    AC_MSG_CHECKING([PHP version])
    if test "$PHP_MAJOR_VERSION" -lt 8; then
        AC_MSG_ERROR([php_trace requires PHP 8.0 or later])
    fi
    AC_MSG_RESULT([$PHP_MAJOR_VERSION.$PHP_MINOR_VERSION])

    dnl ------------------------------------------------------------------
    dnl Thread safety check
    dnl ------------------------------------------------------------------
    if test "$PHP_THREAD_SAFETY" = "yes"; then
        CXXFLAGS="$CXXFLAGS -DZTS"
        AC_MSG_NOTICE([Building for ZTS (thread-safe) PHP])
    else
        AC_MSG_NOTICE([Building for NTS (non-thread-safe) PHP])
    fi

    dnl ------------------------------------------------------------------
    dnl Source files
    dnl
    dnl PHP_NEW_EXTENSION generates compile rules using $(CC) which does not
    dnl support .cpp files. Instead, we declare the extension with no sources
    dnl and add C++ sources separately via PHP_ADD_SOURCES with the [cxx] flag,
    dnl which generates $(CXX) compile rules.
    dnl ------------------------------------------------------------------
    PHP_NEW_EXTENSION(php_trace, , $ext_shared)
    PHP_ADD_SOURCES([$ext_builddir/src], [span.cpp span_buffer.cpp loki_exporter.cpp php_trace.cpp], [cxx])

    PHP_ADD_INCLUDE([$ext_srcdir/include])

    PHP_SUBST(PHP_TRACE_SHARED_LIBADD)

    dnl ------------------------------------------------------------------
    dnl Link flags
    dnl ------------------------------------------------------------------
    PHP_ADD_LIBRARY(pthread, , PHP_TRACE_SHARED_LIBADD)
fi
