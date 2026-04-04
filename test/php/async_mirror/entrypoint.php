<?php
/**
 * TrueAsync entrypoint for echo/mirror tests.
 *
 * Reads the request body and echoes it back, similar to the standard
 * test/php/mirror fixture but using the async request API.
 *
 * Requires: PHP 8.5+ with TrueAsync + Unit PHP extension.
 *
 * TODO: Adjust \Unit\Request API surface once nxt_php_extension.c
 *       is implemented and the PHP-side class names are finalised.
 */

\Unit\Server::setHandler(function (\Unit\Request $request): void {
    $body = $request->body();
    $request->respond(200, [
        'Content-Type'   => 'text/plain',
        'Content-Length' => (string) strlen($body),
    ], $body);
});
