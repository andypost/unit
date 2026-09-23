<?php

declare(strict_types=1);

namespace Drupal\freeunit\Schedule;

use Drupal\Core\Config\ConfigFactoryInterface;
use Drupal\Core\State\StateInterface;
use Drupal\freeunit\Control\ControlApiClient;

/**
 * Builds, checks and applies the FreeUnit schedule that runs Drupal cron.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.  The schedule shape follows
 * docs/adr/0004-schedules.md section 1 and was accepted by a local unitd
 * built from this tree (nxt_conf_vldt_schedule_members,
 * src/nxt_conf_validation.c:1413).
 *
 * The run is "GET /freeunit/cron" with the key in X-FreeUnit-Cron-Key:
 * - the key is Drupal's own cron key (state system.cron_key), so rotating it
 *   in the UI and re-applying the schedule is the whole procedure;
 * - it is not in the URI, so FreeUnit's info log (which already redacts the
 *   URI after the last "/") and any access log never contain it;
 * - a schedule passes straight to the application
 *   (nxt_conf_vldt_schedule_pass allows only applications/...), so the
 *   listener routes can answer 404 for /freeunit/cron to every client.
 * It still lives in FreeUnit's configuration (GET /config, the state
 * directory), which only root can read.
 */
final class ScheduleConfigGenerator {

  public function __construct(
    private readonly ConfigFactoryInterface $configFactory,
    private readonly StateInterface $state,
  ) {}

  public function name(): string {
    return (string) $this->configFactory->get('freeunit.settings')->get('cron.schedule_name');
  }

  /**
   * The schedule object for /config/schedules/<name>.
   *
   * @return array<string, mixed>
   */
  public function build(?string $host = NULL): array {
    $s = $this->configFactory->get('freeunit.settings');
    $key = (string) $this->state->get('system.cron_key');
    if ($key === '') {
      throw new \LogicException('state system.cron_key is empty; run the installer or drush state:set.');
    }
    $interval = max(60, (int) $s->get('cron.interval'));
    $jitter = min($interval, max(0, (int) $s->get('cron.jitter')));
    $timeout = max(1, min((int) $s->get('cron.timeout'), 2147483));
    $host ??= (string) $s->get('cron.host');

    $pass = 'applications/' . $s->get('control.application');
    if ((string) $s->get('control.target') !== '') {
      $pass .= '/' . $s->get('control.target');
    }

    $headers = ['X-FreeUnit-Cron-Key' => $key];
    if ($host !== '') {
      // Needed when settings.php sets trusted_host_patterns: without Host the
      // run's server name is "localhost" (ADR 0004, R4).
      $headers['Host'] = $host;
    }

    return [
      'pass' => $pass,
      'uri' => '/freeunit/cron',
      'interval' => $interval,
      'jitter' => $jitter,
      'timeout' => $timeout,
      'overlap' => 'skip',
      'headers' => $headers,
    ];
  }

  /**
   * Differences between the live schedule and the generated one.
   *
   * @return string[]
   *   Human-readable problems; empty when the live schedule is current.
   */
  public function check(ControlApiClient $client, ?string $host = NULL): array {
    $want = $this->build($host);
    $live = $client->get('/config/schedules/' . rawurlencode($this->name()));
    if ($live === NULL) {
      return [sprintf('No schedule "%s" in the FreeUnit configuration.', $this->name())];
    }
    $problems = [];
    foreach ($want as $field => $value) {
      if (($live[$field] ?? NULL) != $value) {
        // Never print the key itself.
        $problems[] = $field === 'headers'
          ? 'headers differ (Host or cron key)'
          : sprintf('%s is %s, expected %s', $field, json_encode($live[$field] ?? NULL), json_encode($value));
      }
    }
    $apps = $client->get('/config/applications') ?? [];
    $app = (string) $this->configFactory->get('freeunit.settings')->get('control.application');
    $max = $apps[$app]['processes']['max'] ?? ($apps[$app]['processes'] ?? 1);
    if (is_int($max) && $max < 2) {
      $problems[] = 'the application has fewer than 2 processes; a cron run would block the site (ADR 0004, R5)';
    }
    $limit = $apps[$app]['limits']['timeout'] ?? NULL;
    if (is_int($limit) && $want['timeout'] > $limit) {
      $problems[] = sprintf('schedule timeout %d exceeds the application limits.timeout %d', $want['timeout'], $limit);
    }
    return $problems;
  }

  public function apply(ControlApiClient $client, ?string $host = NULL): void {
    $client->putPath('/config/schedules/' . rawurlencode($this->name()), $this->build($host));
  }

}
