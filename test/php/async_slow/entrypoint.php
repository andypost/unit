<?php
/**
 * TrueAsync entrypoint that simulates a slow (in-flight) request.
 *
 * Sleeps for a configurable number of seconds before responding,
 * controlled by the ?sleep=N query parameter (default 3 s, max 30 s).
 * Used by tests that need to verify graceful shutdown waits for
 * in-flight coroutines to finish before exiting.
 *
 * Requires: PHP 8.5+ with TrueAsync + Unit PHP extension.
 *
 * NOTE: The sleep here must be a coroutine-friendly async sleep so
 * that the scheduler remains responsive to shutdown signals while
 * the handler is suspended. Using the synchronous sleep() would block
 * the event loop and defeat the purpose of TrueAsync mode.
 *
 * TODO: Replace \Async\sleep() with the correct TrueAsync API once
 *       the extension API surface is stable.
 */

\Unit\Server::setHandler(function (\Unit\Request $request): void {
    $params = [];
    parse_str($request->query(), $params);

    $delay = min((int) ($params['sleep'] ?? 3), 30);

    // Write a sentinel file before sleeping so the test process can
    // detect that this handler is genuinely in-flight (not just queued
    // in the kernel TCP buffer).  The path is passed as ?signal=<path>.
    // Writing the worker PID lets the test double-check which process is
    // handling the request.
    if (!empty($params['signal'])) {
        file_put_contents($params['signal'], (string) getmypid());
    }

    // Async sleep: suspends this coroutine without blocking the event
    // loop, allowing other coroutines (and the quit handler) to run.
    \Async\sleep($delay);

    $request->respond(200, ['Content-Type' => 'text/plain'], "done\n");
});
