<?php

declare(strict_types=1);

namespace Drupal\freeunit\Controller;

use Drupal\Core\CronInterface;
use Drupal\Core\DependencyInjection\AutowireTrait;
use Drupal\Core\DependencyInjection\ContainerInjectionInterface;
use Drupal\Core\State\StateInterface;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\HttpKernel\Exception\NotFoundHttpException;

/**
 * Runs cron for a FreeUnit schedule.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.
 *
 * Answers 404, not 403, to anything that is not a keyed loopback request, so
 * the route looks like it does not exist.  A schedule run comes from
 * 127.0.0.1 (src/nxt_router_schedule.c:474).  REMOTE_ADDR is read raw, not
 * through Request::getClientIp(), so a trusted reverse proxy setting cannot
 * make a forwarded request look local.
 */
final class CronController implements ContainerInjectionInterface {

  use AutowireTrait;

  public function __construct(
    private readonly CronInterface $cron,
    private readonly StateInterface $state,
  ) {}

  public function run(Request $request): Response {
    $key = (string) $this->state->get('system.cron_key');
    $given = (string) $request->headers->get('X-FreeUnit-Cron-Key', '');
    $remote = (string) $request->server->get('REMOTE_ADDR');
    if ($key === '' || !hash_equals($key, $given) || !in_array($remote, ['127.0.0.1', '::1'], TRUE)) {
      throw new NotFoundHttpException();
    }
    // Synchronous, unlike automated_cron: the schedule's "overlap": "skip"
    // then really prevents overlapping runs (ADR 0004, R3), and the run's
    // status and duration in FreeUnit's log are cron's own.
    $this->cron->run();
    return new Response('', 204, ['Cache-Control' => 'no-store']);
  }

}
