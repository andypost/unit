<?php

declare(strict_types=1);

namespace Drupal\freeunit\Hook;

use Drupal\Component\Datetime\TimeInterface;
use Drupal\Core\Hook\Attribute\Hook;
use Drupal\freeunit\Cache\StaticCacheTagsInvalidator;

/**
 * Hook implementations for freeunit.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.
 */
final class FreeUnitHooks {

  public function __construct(
    private readonly StaticCacheTagsInvalidator $invalidator,
    private readonly TimeInterface $time,
  ) {}

  /**
   * drupal_flush_all_caches() deletes cache.page with deleteAll(), which no
   * tag invalidation reports; it does invoke hook_cache_flush first.
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

}
