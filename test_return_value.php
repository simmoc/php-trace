<?php
/**
 * test_return_value.php - 测试 php_trace 扩展的返回值捕获功能
 */

// 测试 1: 简单函数返回值
function add($a, $b) {
    return $a + $b;
}

// 测试 2: 字符串函数返回值
function greet($name) {
    return "Hello, " . $name;
}

// 测试 3: 数组函数返回值
function get_users() {
    return ["alice", "bob", "charlie"];
}

// 测试 4: 内部函数返回值
function test_internal() {
    $str = "hello world";
    return strlen($str);
}

// 测试 5: 无返回值函数
function no_return($x) {
    $x = $x + 1;
    // 没有 return 语句
}

// 测试 6: 条件返回
function conditional_return($flag) {
    if ($flag) {
        return "yes";
    } else {
        return "no";
    }
}

// 测试 7: 复杂返回值
function complex_return() {
    return [
        "status" => "ok",
        "data" => [1, 2, 3],
        "meta" => (object)["count" => 3]
    ];
}

echo "=== PHP Trace Return Value Test ===\n\n";

// 执行测试
$result1 = add(10, 20);
echo "1. add(10, 20) = $result1\n";

$result2 = greet("World");
echo "2. greet('World') = $result2\n";

$result3 = get_users();
echo "3. get_users() = " . json_encode($result3) . "\n";

$result4 = test_internal();
echo "4. test_internal() = $result4\n";

$result5 = no_return(42);
echo "5. no_return(42) = " . var_export($result5, true) . "\n";

$result6 = conditional_return(true);
echo "6. conditional_return(true) = $result6\n";

$result7 = complex_return();
echo "7. complex_return() = " . json_encode($result7) . "\n";

// 测试内部函数（如果 trace_internal=1）
echo "\n=== Testing Internal Functions ===\n";
$arr = [1, 2, 3, 4, 5];
$count = count($arr);
echo "count([1,2,3,4,5]) = $count\n";

$str = "hello";
$len = strlen($str);
echo "strlen('hello') = $len\n";

$sub = substr("hello world", 0, 5);
echo "substr('hello world', 0, 5) = $sub\n";

echo "\n=== Test Complete ===\n";
echo "Check Tempo traces for php.return_value attributes\n";
