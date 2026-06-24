# docker/Dockerfile — Multi-version PHP Trace extension builder
#
# Build a specific version:
#   docker build -f docker/Dockerfile \
#     --build-arg PHP_VERSION=8.3 \
#     -t php-trace:8.3 .
#
# Run:
#   docker run --rm php-trace:8.3 php --ri php_trace
#   docker run --rm php-trace:8.3 php /test_trace.php

ARG  PHP_VERSION=8.3
FROM php:${PHP_VERSION}-cli

LABEL maintainer="php-trace"
LABEL php-version="${PHP_VERSION}"

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4-openssl-dev \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Copy extension source
WORKDIR /build
COPY include/      ./include/
COPY src/          ./src/
COPY CMakeLists.txt ./

# Build with CMake (handles C++ compilation correctly, unlike phpize)
RUN mkdir -p build \
    && cd build \
    && cmake .. -DPHP_CONFIG=/usr/local/bin/php-config \
    && cmake --build . -j$(nproc) \
    && cmake --install .

# Copy config and test
COPY php_trace.ini /build/php_trace.ini
COPY test_trace.php /test_trace.php

# Enable the extension
RUN docker-php-ext-enable php_trace \
    && INI_FILE="/usr/local/etc/php/conf.d/docker-php-ext-php_trace.ini" \
    && echo "php_trace.enabled=1" >> "${INI_FILE}" \
    && echo "php_trace.loki.endpoint=http://localhost:3100/loki/api/v1/push" >> "${INI_FILE}" \
    && echo "php_trace.service_name=php-\${PHP_VERSION}-app" >> "${INI_FILE}" \
    && echo "php_trace.sample_rate=1.0" >> "${INI_FILE}"

# Verify installation
RUN php -m | grep php_trace && php --ri php_trace

# Default: show extension info
CMD ["php", "--ri", "php_trace"]
