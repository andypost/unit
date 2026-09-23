<?php
/*
 * Echoes the inbound HTTP_TRACEPARENT (if any) back as a response header,
 * so the test can see exactly what $_SERVER carries in the PHP SAPI --
 * not just what Unit's own response carries.
 */
$traceparent = $_SERVER['HTTP_TRACEPARENT'] ?? '';
header('X-Seen-Traceparent: ' . $traceparent);
header('Content-Length: 0');
?>
