# php-trace — PHP Trace Extension with Loki/Tempo Support

一个用 C++ 实现的 PHP trace 跟踪扩展，自动捕获函数调用的时序信息，并通过 HTTP 推送到 [Grafana Loki](https://grafana.com/oss/loki/) 或 [Grafana Tempo](https://grafana.com/oss/tempo/) 进行存储和可视化。

## 架构

```
┌──────────────────────────────────────────────────────────────────┐
│                        PHP Runtime                               │
│                                                                  │
│  zend_execute_ex (Hook)                                         │
│       │                    ▲                                     │
│       ▼                    │                                     │
│  [Span Start] ──► 用户函数执行 ──► [Span End]                    │
│                                                                  │
│       │ push                                                      │
│       ▼                                                          │
│  ┌─────────────────────────────────────────┐                     │
│  │   Thread-Safe Ring Buffer (65536 slots)  │                     │
│  │   SPSC + Mutex for multi-producer        │                     │
│  └──────────────────┬──────────────────────┘                     │
│                     │ drain (batch)                               │
│                     ▼                                             │
│  ┌─────────────────────────────────────────┐                     │
│  │   Loki Exporter (Background Thread)      │                     │
│  │   - JSON payload builder                │                     │
│  │   - HTTP POST /loki/api/v1/push          │                     │
│  │   - Retry + error handling              │                     │
│  └──────────────────┬──────────────────────┘                     │
└─────────────────────┼────────────────────────────────────────────┘
                      │ HTTP
                      ▼
              ┌──────────────┐     ┌──────────────┐
              │  Loki (3100) │────▶│  Grafana      │
              │  日志存储      │     │  可视化看板    │
              └──────────────┘     └──────────────┘
```

## 功能特性

- **自动函数追踪** — 通过 hook `zend_execute_ex` 自动捕获函数调用
- **默认聚焦阻塞型调用** — 默认只追踪 `mysqli_*`、`curl_*`、`PDO`、`fsockopen`、`file_get_contents` 等可能引发阻塞/IO 的内部调用
- **W3C Trace Context** — 自动解析 `traceparent` HTTP Header，支持分布式链路追踪
- **手动 Span API** — 提供 `php_trace_create_span()` / `php_trace_finalize_span()` 用于自定义 span
- **异常捕获** — 自动标记异常函数调用为 `ERROR` 状态
- **采样控制** — 支持概率采样，降低生产环境开销
- **正则过滤** — 通过 `include_pattern` / `exclude_pattern` 精确控制追踪范围
- **多后端导出** — 支持 Loki 和 Tempo，切换通过 `PHP_TRACE_EXPORTER` / `PHP_TRACE_TEMPO_ENDPOINT`
- **批量推送** — 后台线程批量发送，不阻塞 PHP 请求处理
- **重试机制** — HTTP 请求失败自动重试
- **线程安全** — 支持 ZTS（多线程 SAPI）和 NTS

## 系统要求

| 组件 | 版本 |
|------|------|
| PHP | 8.0+ (8.1 / 8.2 / 8.3) |
| 编译器 | GCC 8+ / Clang 7+ / MSVC 2019+ |
| CMake | 3.14+ (可选) |
| libcurl | 7.x+ (开发包) |
| Loki | 2.9+ |
| Grafana | 10+ (用于可视化) |

## 快速开始

### 1. 启动监控栈（Loki + Tempo + Grafana）

```bash
docker compose up -d loki tempo grafana
```

- Grafana: http://localhost:3000 (admin / admin)
- Loki: http://localhost:3100
- Tempo: http://localhost:3200
- Tempo OTLP/HTTP: http://localhost:4318

### 2. 构建 PHP 扩展

**方式 A: phpize（推荐）**

```bash
# 安装依赖 (Ubuntu/Debian)
sudo apt install php8.3-dev libcurl4-openssl-dev build-essential

# 构建
cd php-trace
phpize
./configure --enable-php-trace
make -j$(nproc)
sudo make install
```

**方式 B: CMake**

```bash
mkdir build && cd build
cmake .. -DPHP_CONFIG=/usr/bin/php-config
cmake --build . -j$(nproc)
sudo cmake --install .
```

### 3. 配置 PHP

```bash
# 复制配置文件
sudo cp php_trace.ini /etc/php/8.3/mods-available/php_trace.ini
sudo phpenmod php_trace

# 或者直接编辑 php.ini 追加
echo "extension=php_trace.so" >> /etc/php/8.3/cli/php.ini
echo "php_trace.enabled=1"    >> /etc/php/8.3/cli/php.ini
echo "php_trace.loki.endpoint=http://localhost:3100/loki/api/v1/push" >> /etc/php/8.3/cli/php.ini
# 若要发送到 Tempo：
echo "PHP_TRACE_EXPORTER=tempo" >> /etc/environment
echo "PHP_TRACE_TEMPO_ENDPOINT=http://localhost:3200/api/v2/spans" >> /etc/environment
```

### 4. 验证安装

```bash
php -m | grep php_trace
php --ri php_trace
```

### 5. 运行测试

```bash
php test_trace.php
```

## 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `php_trace.enabled` | `1` | 总开关 |
| `php_trace.service_name` | `php-app` | 服务名称 |
| `php_trace.loki.endpoint` | `http://localhost:3100/loki/api/v1/push` | Loki push API 地址 |
| `PHP_TRACE_EXPORTER` | `loki` | 导出后端 (`loki` / `tempo`) |
| `PHP_TRACE_TEMPO_ENDPOINT` | `http://tempo:3200/api/v2/spans` | Tempo OTLP/HTTP 地址 |
| `php_trace.loki.username` | `` | Basic Auth 用户名 |
| `php_trace.loki.password` | `` | Basic Auth 密码 |
| `php_trace.loki.tenant_id` | `` | 多租户 X-Scope-OrgID |
| `php_trace.loki.batch_size` | `512` | 单次 HTTP 最大 span 数 |
| `php_trace.loki.flush_interval` | `5` | 刷新间隔（秒） |
| `php_trace.sample_rate` | `1.0` | 采样率 (0.0 ~ 1.0) |
| `php_trace.max_spans` | `65536` | Ring buffer 容量 |
| `php_trace.capture_args` | `0` | 是否捕获函数参数 |
| `php_trace.capture_return` | `0` | 是否捕获返回值 |
| `php_trace.max_arg_length` | `256` | 参数值最大长度 |
| `php_trace.trace_user` | `0` | 追踪用户函数 |
| `php_trace.trace_internal` | `1` | 追踪内置函数 |
| `php_trace.include_pattern` | `^(mysqli_|mysql_|PDO::|PDOStatement::|curl_|fsockopen|stream_socket_client|file_get_contents|fopen|stream_socket_|socket_|redis_|ldap_|pg_|sqlite_)` | 默认只匹配常见阻塞/IO 调用 |
| `php_trace.exclude_pattern` | `` | 跳过匹配 regex 的函数 |
| `php_trace.max_depth` | `0` | 最大嵌套深度 (0=无限制) |

## PHP API

### `php_trace_status(): array`

获取扩展状态和统计信息。

```php
$status = php_trace_status();
// {
//   "enabled": true,
//   "total_pushed": 1523,
//   "total_dropped": 0,
//   "spans_exported": 1024,
//   "trace_id": "a1b2c3d4e5f6...",
//   ...
// }
```

### `php_trace_create_span(string $name, array $attrs = []): ?string`

创建手动 span，返回 span ID。

```php
$spanId = php_trace_create_span('cache.get', [
    'cache.key'   => 'user:123',
    'cache.hit'   => true,
    'cache.type'  => 'redis',
]);
```

### `php_trace_finalize_span(string $spanId, int $status = 1): bool`

完成手动 span 并推送到 buffer。

```php
php_trace_finalize_span($spanId, 1); // 1=OK, 2=ERROR
```

## Loki 数据格式

每个 span 在 Loki 中是一条 log line，格式如下：

**Labels:**
```json
{
  "job": "php-trace",
  "service": "my-php-app",
  "operation": "App\\Service\\OrderService::createOrder",
  "kind": "0"
}
```

**Log Line (JSON):**
```json
{
  "trace_id": "a1b2c3d4e5f67890abcdef1234567890",
  "span_id": "1234567890abcdef",
  "parent_span_id": "fedcba0987654321",
  "operation_name": "App\\Service\\OrderService::createOrder",
  "service_name": "my-php-app",
  "kind": 0,
  "start_time_ns": 1719234567890123456,
  "end_time_ns": 1719234567895123456,
  "duration_ns": 5000000,
  "duration_ms": 5.0,
  "status": "OK",
  "depth": 1,
  "is_root": false,
  "attributes": {
    "order.id": "ord_12345",
    "http.method": "POST"
  }
}
```

## Grafana 查询示例

```logql
# 查看所有 trace
{job="php-trace"} | json

# 只看错误的 span
{job="php-trace"} | json | status = "ERROR"

# 只看某个服务的
{job="php-trace", service="my-php-app"} | json

# 只看耗时 > 100ms 的
{job="php-trace"} | json | duration_ms > 100

# 查看某个 trace 的所有 span
{job="php-trace"} | json | trace_id = "a1b2c3d4..."

# 按操作名称聚合
sum by (operation) (count_over_time({job="php-trace"} | json [5m]))
```

## 生产环境建议

1. **采样率**: 高流量场景建议使用 `sample_rate = 0.1` (10%) 或更低
2. **过滤**: 配合 `exclude_pattern` 排除框架内部函数，减少噪音
3. **路径过滤**:
   ```ini
   php_trace.include_pattern = "^App\\\\"     # 只追踪业务代码
   php_trace.exclude_pattern = "^(Composer\\\\|vendor\\\\|array_)"  # 排除框架
   ```
4. **Buffer 容量**: 按 QPS * 平均调用深度 * 刷新间隔估算
   - 例如: 1000 QPS * 10 calls * 5s = 50000 → `max_spans = 65536`
5. **连接池**: Loki 建议使用负载均衡器前端，避免单点瓶颈

## 项目结构

```
php-trace/
├── include/
│   ├── span.h           # Span 数据模型
│   ├── span_buffer.h    # 线程安全 Ring Buffer
│   ├── loki_exporter.h  # Loki HTTP 导出器
│   └── php_trace.h      # PHP 扩展主头文件
├── src/
│   ├── span.cpp
│   ├── span_buffer.cpp
│   ├── loki_exporter.cpp
│   └── php_trace.cpp    # 扩展核心 + Hook
├── config.m4            # Unix/Linux 构建 (phpize)
├── config.w32           # Windows 构建
├── CMakeLists.txt       # CMake 构建
├── php_trace.ini        # 配置模板
├── docker-compose.yml   # Loki + Grafana
├── loki-config.yaml     # Loki 配置
├── grafana-datasources.yaml
├── grafana-dashboard.json
├── test_trace.php       # 测试脚本
└── README.md
```

## 许可证

MIT License
