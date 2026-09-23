<?php
/*
 * A small PHP app for test/test_schedules_php.py.
 *
 * On every hit it appends one JSON line to the file named by the
 * SCHEDULE_LOG environment variable when the request starts, and one more
 * when it is about to answer -- mirroring test/python/schedule/wsgi.py, so
 * the same wait_for_starts()/records()/assert_no_concurrency() helpers work
 * for both languages.
 *
 * Query string knobs:
 *   sleep=N     sleep N seconds before answering (float)
 *   status=N    answer with HTTP status N instead of 200
 *   finish=1    call fastcgi_finish_request() after sending the response,
 *               then keep sleeping for "sleep" seconds in the background.
 *               This is what "automated_cron"-style endpoints do; the
 *               dedicated Drupal /cron/{key} endpoint does not.
 */

function record($event, $extra = [])
{
    $log = getenv('SCHEDULE_LOG');

    if ($log === false) {
        return;
    }

    $rec = array_merge(
        [
            'event' => $event,
            'time' => microtime(true),
            'pid' => getmypid(),
            'uri' => $_SERVER['REQUEST_URI'] ?? null,
            'query' => $_SERVER['QUERY_STRING'] ?? null,
            'host' => $_SERVER['HTTP_HOST'] ?? null,
            'user_agent' => $_SERVER['HTTP_USER_AGENT'] ?? null,
            'x_cron' => $_SERVER['HTTP_X_CRON'] ?? null,
            'server_name' => $_SERVER['SERVER_NAME'] ?? null,
            'server_port' => $_SERVER['SERVER_PORT'] ?? null,
            'remote_addr' => $_SERVER['REMOTE_ADDR'] ?? null,
            'method' => $_SERVER['REQUEST_METHOD'] ?? null,
        ],
        $extra
    );

    // Match Python's json.dumps() + "\n" line framing, appended atomically
    // enough for these tests: short writes, one process at a time in
    // practice because "processes": {"max"} is generous relative to the
    // schedule's own concurrency.
    file_put_contents($log, json_encode($rec) . "\n", FILE_APPEND | LOCK_EX);
}

record('start');

$sleep = isset($_GET['sleep']) ? (float) $_GET['sleep'] : 0.0;
$status = isset($_GET['status']) ? (int) $_GET['status'] : 200;
$finish = isset($_GET['finish']) && $_GET['finish'] === '1';

$body = "ran " . ($_SERVER['REQUEST_URI'] ?? '') . "\n";

http_response_code($status);
header('Content-Type: text/plain');

if ($finish) {
    // Send the response and detach, exactly as Drupal's automated_cron
    // relies on PHP doing.  The schedule run is considered complete as
    // soon as this returns -- see docs/schedules.md, "fastcgi_finish_
    // request() makes a run finish early".
    echo $body;
    record('end', ['status' => $status, 'finished_early' => true]);
    fastcgi_finish_request();

    // Work that continues after the client (and the schedule run) has
    // already been told the request is done.
    if ($sleep > 0) {
        usleep((int) ($sleep * 1000000));
    }

    record('detached_end', ['status' => $status]);
    exit;
}

if ($sleep > 0) {
    usleep((int) ($sleep * 1000000));
}

record('end', ['status' => $status]);

echo $body;
