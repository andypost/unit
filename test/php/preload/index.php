<?php

$pid = getmypid();

header('X-Pid: ' . $pid);

if (!function_exists('opcache_is_script_cached')) {
    header('X-OPcache: -1');
} else {
    $st = function_exists('opcache_get_status') ? @opcache_get_status(false) : null;
    $enabled = is_array($st) && !empty($st['opcache_enabled']);
    header('X-OPcache: ' . ($enabled ? '1' : '-1'));

    /* Preload visibility probes. */
    header('X-Class: ' . (class_exists('HelloPreloaded', false) ? 'yes' : 'no'));
    header('X-Fn: ' . (function_exists('preloaded_fn') ? 'yes' : 'no'));

    /* Warmup probes. Query-string list of files to check. */
    if (isset($_GET['check'])) {
        $files = explode(',', $_GET['check']);
        $out = [];
        foreach ($files as $rel) {
            $abs = __DIR__ . '/' . $rel;
            $out[] = $rel . '=' . (opcache_is_script_cached($abs) ? '1' : '0');
        }
        header('X-Warmup: ' . implode(';', $out));
    }
}

echo "OK\n";
