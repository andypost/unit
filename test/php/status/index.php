<?php
/**
 * Basic PHP script for status API testing
 * Returns simple response for testing request counters and memory
 */

// Simple response
echo "Status test OK\n";

// Optional: allocate memory if requested
if (isset($_SERVER['HTTP_X_ALLOC'])) {
    $size = (int)$_SERVER['HTTP_X_ALLOC'];
    if ($size > 0) {
        $data = str_repeat('x', $size * 1024);
        echo "Allocated {$size}KB\n";
    }
}

// Optional: trigger GC if requested
if (isset($_SERVER['HTTP_X_TRIGGER_GC']) && function_exists('gc_collect_cycles')) {
    gc_collect_cycles();
    echo "GC triggered\n";
}
