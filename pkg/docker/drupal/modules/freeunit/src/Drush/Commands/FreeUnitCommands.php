<?php

declare(strict_types=1);

namespace Drupal\freeunit\Drush\Commands;

use Drupal\freeunit\Cache\StaticCacheTagsInvalidator;
use Drupal\freeunit\Control\ControlApiClient;
use Drupal\freeunit\Schedule\ScheduleConfigGenerator;
use Drupal\freeunit\StaticCache\RouteConfigGenerator;
use Drush\Attributes as CLI;
use Drush\Commands\AutowireTrait;
use Drush\Commands\DrushCommands;
use Consolidation\AnnotatedCommand\Hooks\HookManager;

/**
 * Drush commands for FreeUnit.
 *
 * UNVERIFIED PROTOTYPE: never run.  Written against the Drush 13 attribute
 * API (Drush\Attributes, AutowireTrait); check before use.
 *
 * These are the only place the module talks to the control socket: run
 * them as the user that owns it (root in the dev kit image).
 */
final class FreeUnitCommands extends DrushCommands {

  use AutowireTrait;

  public function __construct(
    private readonly ControlApiClient $client,
    private readonly ScheduleConfigGenerator $schedules,
    private readonly RouteConfigGenerator $routes,
    private readonly StaticCacheTagsInvalidator $invalidator,
  ) {
    parent::__construct();
  }

  #[CLI\Command(name: 'freeunit:status')]
  #[CLI\Help(description: 'Show FreeUnit /status for the Drupal application.')]
  public function status(): void {
    $app = $this->appName();
    $this->io()->writeln(json_encode(
      $this->client->get('/status/applications/' . rawurlencode($app)),
      JSON_PRETTY_PRINT,
    ));
  }

  #[CLI\Command(name: 'freeunit:schedule')]
  #[CLI\Help(description: 'Check, or with --apply install, the FreeUnit schedule that runs cron.')]
  #[CLI\Option(name: 'apply', description: 'PUT the schedule to the control API.')]
  #[CLI\Option(name: 'host', description: 'Host header for the run.')]
  public function schedule(array $options = ['apply' => FALSE, 'host' => NULL]): int {
    if ($options['apply']) {
      $this->schedules->apply($this->client, $options['host']);
      $this->logger()->success(dt('Schedule @n applied.', ['@n' => $this->schedules->name()]));
    }
    $problems = $this->schedules->check($this->client, $options['host']);
    foreach ($problems as $p) {
      $this->logger()->warning($p);
    }
    return $problems === [] ? self::EXIT_SUCCESS : self::EXIT_FAILURE;
  }

  #[CLI\Command(name: 'freeunit:routes')]
  #[CLI\Help(description: 'Print the drupal_page route for the static cache (JSON), or PUT it with --apply.')]
  #[CLI\Option(name: 'host', description: 'Comma-separated host names.')]
  #[CLI\Option(name: 'apply', description: 'PUT to /config/routes/drupal_page.')]
  public function routes(array $options = ['host' => NULL, 'apply' => FALSE]): void {
    $hosts = array_filter(explode(',', (string) $options['host']));
    // A real implementation fetches the front page anonymously and takes
    // the reproduced header values from that response.
    $routes = $this->routes->build($hosts, [
      'Cache-Control' => 'max-age=' . (int) \Drupal::config('system.performance')->get('cache.page.max_age') . ', public',
      'Vary' => 'Cookie, Accept-Encoding',
      'X-Content-Type-Options' => 'nosniff',
      'X-Frame-Options' => 'SAMEORIGIN',
    ]);
    if ($options['apply']) {
      $this->client->putPath('/config/routes/drupal_page', $routes);
      return;
    }
    $this->io()->writeln(json_encode($routes, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES));
  }

  #[CLI\Command(name: 'freeunit:restart')]
  #[CLI\Help(description: 'Gracefully restart the Drupal application: fresh workers, empty OPcache, preload re-run.')]
  public function restart(): void {
    $this->client->restartApplication($this->appName());
    $this->logger()->success(dt('Application @a restarted.', ['@a' => $this->appName()]));
  }

  #[CLI\Command(name: 'freeunit:cache-purge')]
  #[CLI\Help(description: 'Delete every file of the static page cache.')]
  public function purge(): void {
    $this->invalidator->purge();
  }

  /**
   * After "drush deploy", restart the workers so no worker keeps code or
   * OPcache entries from before the deployment.
   */
  #[CLI\Hook(type: HookManager::POST_COMMAND_HOOK, target: 'deploy')]
  public function postDeploy(): void {
    if ($this->client->isAvailable()) {
      $this->restart();
    }
    else {
      $this->logger()->notice(dt('FreeUnit control socket not writable; skipping the application restart.'));
    }
  }

  private function appName(): string {
    return (string) \Drupal::config('freeunit.settings')->get('control.application');
  }

}
