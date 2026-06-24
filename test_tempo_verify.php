<?php
/**
 * Tempo 数据上报端到端验证脚本
 */

echo "=== Tempo End-to-End Verification ===\n\n";

// 1. Check extension
echo "[1] Extension loaded: " . (extension_loaded('php_trace') ? 'YES' : 'NO') . "\n";
$status = php_trace_status();
echo "    exporter: " . ($status['exporter_running'] ? 'running' : 'stopped') . "\n\n";

// 2. Run some traced functions
function slow_calc($n) {
    if ($n <= 1) return 1;
    usleep(100);
    return slow_calc($n - 1) + slow_calc($n - 2);
}

function http_request($url) {
    $start = microtime(true);
    // Simulate HTTP call
    usleep(5000);
    $end = microtime(true);
    return ['url' => $url, 'elapsed_ms' => ($end - $start) * 1000];
}

function db_query($sql) {
    usleep(3000);
    return ['sql' => $sql, 'rows' => 42];
}

echo "[2] Executing traced functions...\n";
$r1 = slow_calc(10);
$r2 = http_request('http://api.example.com/users');
$r3 = db_query('SELECT * FROM users LIMIT 10');
echo "    slow_calc(10) = $r1\n";
echo "    http_request = " . json_encode($r2) . "\n";
echo "    db_query = " . json_encode($r3) . "\n\n";

// 3. Check stats after tracing
$post = php_trace_status();
echo "[3] After tracing:\n";
echo "    total_pushed: " . $post['total_pushed'] . "\n";
echo "    total_dropped: " . $post['total_dropped'] . "\n";
echo "    total_drained: " . $post['total_drained'] . "\n";
echo "    spans_exported: " . $post['spans_exported'] . "\n";
echo "    batches_sent: " . $post['batches_sent'] . "\n";
echo "    batches_failed: " . $post['batches_failed'] . "\n\n";

// 4. Wait for exporter to flush (batch interval is 5s by default)
echo "[4] Waiting for exporter to flush (10s)...\n";
sleep(10);

$final = php_trace_status();
echo "[5] Final stats:\n";
echo "    total_pushed: " . $final['total_pushed'] . "\n";
echo "    total_dropped: " . $final['total_dropped'] . "\n";
echo "    total_drained: " . $final['total_drained'] . "\n";
echo "    spans_exported: " . $final['spans_exported'] . "\n";
echo "    batches_sent: " . $final['batches_sent'] . "\n";
echo "    batches_failed: " . $final['batches_failed'] . "\n\n";

// 5. Verify Tempo received data via curl
echo "[6] Querying Tempo for our trace_id...\n";
$trace_id = $final['trace_id'];
$tempo_url = "http://172.18.0.3:3200/api/traces/" . $trace_id;

$ch = curl_init($tempo_url);
curl_setopt_array($ch, [
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_TIMEOUT => 5,
    CURLOPT_HTTPHEADER => ['Accept: application/json'],
]);
$response = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);

echo "    HTTP $http_code — trace_id: $trace_id\n";
if ($http_code === 200 && $response) {
    $data = json_decode($response, true);
    if ($data && !empty($data['batches'])) {
        $span_count = 0;
        foreach ($data['batches'] as $batch) {
            foreach (($batch['scopeSpans'] ?? []) as $ss) {
                $span_count += count($ss['spans'] ?? []);
            }
        }
        echo "    Traces found: YES ($span_count spans)\n";
    } else {
        echo "    Raw response: " . substr($response, 0, 200) . "\n";
    }
} else {
    echo "    Response: " . substr($response, 0, 300) . "\n";
}

echo "\n=== " . ($final['batches_failed'] == 0 && $http_code == 200 ? 'VERIFIED — Tempo export working!' : 'FAILED — check errors above') . " ===\n";
