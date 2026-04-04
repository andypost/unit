<?php
/**
 * TrueAsync entrypoint for graceful shutdown tests.
 *
 * This file is loaded once when the PHP worker starts (not per-request).
 * It registers a request handler callback with the Unit PHP extension,
 * which is then called by nxt_php_request_handler_async() for each
 * incoming HTTP request.
 *
 * Requires: PHP 8.5+ with TrueAsync extension and Unit PHP extension
 *           (nxt_php_extension.c / nxt_php_extension_init()).
 *
 * TODO: Replace \Unit\Server::setHandler() with the actual API once
 *       nxt_php_extension.c is implemented. The C side expects
 *       nxt_php_request_callback to be set to a callable/object.
 */

\Unit\Server::setHandler(function (\Unit\Request $request): void {
    // Minimal handler: return 200 OK with a known body so tests can
    // verify the worker is alive and serving requests.
    $request->respond(200, ['Content-Type' => 'text/plain'], "OK\n");
});
