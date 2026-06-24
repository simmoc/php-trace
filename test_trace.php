<?php
/**
 * test_trace.php — PHP Trace Extension Test Script
 *
 * Usage: php -d extension=php_trace -d php_trace.enabled=1 test_trace.php
 *
 * Prerequisites:
 *   1. php_trace extension installed and enabled
 *   2. Grafana Loki running (default: http://localhost:3100)
 *   3. Or use the noop mode (php_trace.loki.endpoint="")
 */

/**
 * Test 1: Basic function tracing
 */
function fibonacci(int $n): int
{
    if ($n <= 1) {
        return $n;
    }
    return fibonacci($n - 1) + fibonacci($n - 2);
}

/**
 * Test 2: Class method tracing
 */
class OrderService
{
    private string $name;

    public function __construct(string $name)
    {
        $this->name = $name;
    }

    public function createOrder(array $items): array
    {
        $this->validateItems($items);
        $total = $this->calculateTotal($items);
        
        return [
            'id'     => uniqid('order_'),
            'items'  => $items,
            'total'  => $total,
            'status' => 'pending',
        ];
    }

    private function validateItems(array $items): void
    {
        foreach ($items as $item) {
            if (!isset($item['price']) || $item['price'] <= 0) {
                throw new \InvalidArgumentException("Invalid item price");
            }
        }
    }

    private function calculateTotal(array $items): float
    {
        $total = 0;
        foreach ($items as $item) {
            $total += $item['price'] * ($item['qty'] ?? 1);
        }
        return round($total, 2);
    }

    public function getName(): string
    {
        return $this->name;
    }
}

/**
 * Test 3: Manual span creation
 */
function tracedDatabaseQuery(string $sql): array
{
    // Create manual span
    $spanId = php_trace_create_span('db.query', [
        'db.system'    => 'mysql',
        'db.statement' => $sql,
        'db.name'      => 'app_db',
    ]);

    // Simulate query
    usleep(random_int(1000, 5000));
    $result = [
        ['id' => 1, 'name' => 'Alice'],
        ['id' => 2, 'name' => 'Bob'],
    ];

    // Finalize span
    php_trace_finalize_span($spanId, 1); // 1 = OK

    return $result;
}

/**
 * Test 4: Exception trace
 */
function riskyOperation(): void
{
    throw new \RuntimeException("Something went wrong!");
}

// ===========================================================================
// Run tests
// ===========================================================================
echo "========================================\n";
echo "  PHP Trace Extension Test\n";
echo "========================================\n\n";

// Check status
echo "[1] Extension Status:\n";
$status = php_trace_status();
echo json_encode($status, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n\n";

// Test 1: Recursive function
echo "[2] Testing recursive tracing (fibonacci 10)...\n";
$start = hrtime(true);
$result = fibonacci(10);
$elapsed = (hrtime(true) - $start) / 1e6;
echo "    Result: {$result}, Time: " . round($elapsed, 2) . "ms\n\n";

// Test 2: Class methods
echo "[3] Testing class method tracing...\n";
$service = new OrderService("order-service-1");
$order = $service->createOrder([
    ['name' => 'Widget A', 'price' => 19.99, 'qty' => 2],
    ['name' => 'Widget B', 'price' => 49.99, 'qty' => 1],
]);
echo "    Order created: id={$order['id']}, total=\${$order['total']}\n";
echo "    Service: {$service->getName()}\n\n";

// Test 3: Manual span
echo "[4] Testing manual span creation...\n";
$dbResult = tracedDatabaseQuery('SELECT * FROM users WHERE status = 1');
echo "    DB rows: " . count($dbResult) . "\n\n";

// Test 4: Exception trace
echo "[5] Testing exception trace...\n";
try {
    riskyOperation();
} catch (\RuntimeException $e) {
    echo "    Caught exception: {$e->getMessage()}\n";
    echo "    (This span will have status=ERROR in Loki)\n\n";
}

// Final status
echo "[6] Final Status:\n";
$final = php_trace_status();
echo "    Spans pushed:  {$final['total_pushed']}\n";
echo "    Spans dropped: {$final['total_dropped']}\n";
echo "    Spans drained: {$final['total_drained']}\n";
echo "    Sample rate:   {$final['sample_rate']}\n";
echo "\n";

echo "========================================\n";
echo "  Test Complete.\n";
echo "  Check Loki at: http://localhost:3100\n";
echo "  Labels: {job=\"php-trace\"}\n";
echo "========================================\n";
