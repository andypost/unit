<?php

declare(strict_types=1);

namespace Drupal\freeunit\Hook;

use Drupal\Component\Datetime\TimeInterface;
use Drupal\Core\Config\ConfigFactoryInterface;
use Drupal\Core\Extension\ModuleHandlerInterface;
use Drupal\Core\Extension\Requirement\RequirementSeverity;
use Drupal\Core\Hook\Attribute\Hook;
use Drupal\Core\StringTranslation\StringTranslationTrait;
use Drupal\freeunit\Cache\StaticCacheTagsInvalidator;

/**
 * Hook implementations for freeunit.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.  Classes in src/Hook are
 * registered and autowired by core (11.1+).
 */
final class FreeUnitHooks {

  use StringTranslationTrait;

  public function __construct(
    private readonly StaticCacheTagsInvalidator $invalidator,
    private readonly TimeInterface $time,
    private readonly ConfigFactoryInterface $configFactory,
    private readonly ModuleHandlerInterface $moduleHandler,
  ) {}

  /**
   * Purges on a full flush.
   *
   * The function drupal_flush_all_caches() deletes cache.page with
   * deleteAll(), which no tag invalidation reports; it does invoke
   * hook_cache_flush first.
   */
  #[Hook('cache_flush')]
  public function cacheFlush(): void {
    $this->invalidator->purge();
  }

  /**
   * Pages whose response had a future Expires header.
   */
  #[Hook('cron')]
  public function cron(): void {
    $this->invalidator->deleteExpired($this->time->getRequestTime());
  }

  /**
   * Status report rows (hook_runtime_requirements, 11.2+).
   *
   * @return array<string, array<string, mixed>>
   */
  #[Hook('runtime_requirements')]
  public function runtimeRequirements(): array {
    $requirements = [];

    // FreeUnit's SAPI registers as "cli-server" (src/nxt_php_sapi.c:299) and
    // reports itself in SERVER_SOFTWARE as "Unit/<version>"
    // (src/nxt_php_sapi.c:1549, src/nxt_main.h:15).
    $software = (string) ($_SERVER['SERVER_SOFTWARE'] ?? '');
    $on_unit = str_starts_with($software, 'Unit/');
    $requirements['freeunit_server'] = [
      'title' => $this->t('FreeUnit'),
      'value' => $on_unit ? $software : $this->t('Not running on FreeUnit'),
      'severity' => $on_unit ? RequirementSeverity::OK : RequirementSeverity::Warning,
    ];

    if ($on_unit) {
      $finish = function_exists('fastcgi_finish_request');
      $requirements['freeunit_finish_request'] = [
        'title' => $this->t('FreeUnit post-response work'),
        'value' => $finish
          ? $this->t('fastcgi_finish_request() available: kernel.terminate runs after the response is sent')
          : $this->t('fastcgi_finish_request() missing'),
        'severity' => $finish ? RequirementSeverity::OK : RequirementSeverity::Warning,
      ];
    }

    $settings = $this->configFactory->get('freeunit.settings');
    if ($settings->get('static_cache.enabled')) {
      $dir = (string) $settings->get('static_cache.directory');
      $ok = is_dir($dir) && is_writable($dir);
      $requirements['freeunit_static_cache'] = [
        'title' => $this->t('FreeUnit static page cache'),
        'value' => $ok ? $dir : $this->t('@dir is missing or not writable by the PHP workers', ['@dir' => $dir]),
        'severity' => $ok ? RequirementSeverity::OK : RequirementSeverity::Error,
      ];
    }

    if ($this->moduleHandler->moduleExists('automated_cron')) {
      $requirements['freeunit_cron'] = [
        'title' => $this->t('FreeUnit cron'),
        'value' => $this->t('automated_cron is enabled; with a FreeUnit schedule (drush freeunit:schedule --apply) it can be uninstalled.'),
        'severity' => RequirementSeverity::Info,
      ];
    }

    return $requirements;
  }

}
