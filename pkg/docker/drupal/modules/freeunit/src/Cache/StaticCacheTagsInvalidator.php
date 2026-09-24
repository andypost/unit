<?php

declare(strict_types=1);

namespace Drupal\freeunit\Cache;

use Drupal\Core\Cache\CacheTagsInvalidatorInterface;
use Drupal\freeunit\StaticCache\StaticCacheFiles;
use Drupal\freeunit\StaticCache\StaticCacheIndex;
use Symfony\Component\EventDispatcher\EventSubscriberInterface;
use Symfony\Component\HttpKernel\KernelEvents;

/**
 * Deletes static cache files when their cache tags are invalidated.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.
 *
 * Collected by cache_tags.invalidator through the cache_tags_invalidator
 * service tag, like every other invalidator in core.
 *
 * Invalidations often happen inside a database transaction (an entity save).
 * If the files were deleted only then, a concurrent anonymous request could
 * read the pre-commit data, render, and write the file again after the
 * delete.  So the tags are deleted twice: at once, and again at the end of
 * this request (kernel.terminate), after the transaction has committed.  The
 * writer's own re-check against cache.page closes the rest of the window.
 */
final class StaticCacheTagsInvalidator implements CacheTagsInvalidatorInterface, EventSubscriberInterface {

  /**
   * Tags invalidated during this request, for the second pass.
   *
   * @var array<string, true>
   */
  private array $pending = [];

  /**
   * Set by uninstall(): the index table is about to be dropped.
   */
  private bool $uninstalled = FALSE;

  public function __construct(
    private readonly StaticCacheIndex $index,
    private readonly StaticCacheFiles $files,
  ) {}

  public static function getSubscribedEvents(): array {
    // Before the writer (-100), so a page rendered in this same request is
    // not written after its own tags were invalidated.
    return [KernelEvents::TERMINATE => [['onTerminate', -50]]];
  }

  /**
   * {@inheritdoc}
   */
  public function invalidateTags(array $tags): void {
    if ($this->uninstalled) {
      return;
    }
    $this->deleteFor($tags);
    foreach ($tags as $tag) {
      $this->pending[$tag] = TRUE;
    }
  }

  /**
   * Deletes every file; called from hook_cache_flush.
   */
  public function purge(): void {
    $this->files->purge();
    $this->index->forgetAll();
  }

  /**
   * Purges, then ignores invalidations for the rest of this request.
   *
   * Called from hook_uninstall.  The module's table is dropped right after
   * it, while this service stays in the container until the uninstall saves
   * core.extension, which invalidates tags.
   */
  public function uninstall(): void {
    $this->purge();
    $this->uninstalled = TRUE;
  }

  public function onTerminate(): void {
    if ($this->pending !== [] && !$this->uninstalled) {
      $tags = array_keys($this->pending);
      $this->pending = [];
      $this->deleteFor($tags);
    }
  }

  /**
   * Expired files; called from cron (hook_cron in src/Hook/FreeUnitHooks.php).
   */
  public function deleteExpired(int $now): void {
    $files = $this->index->expiredFiles($now);
    $this->files->delete($files);
    $this->index->forget($files);
  }

  /**
   * @param string[] $tags
   */
  private function deleteFor(array $tags): void {
    $files = $this->index->filesForTags($tags);
    if ($files !== []) {
      $this->files->delete($files);
      $this->index->forget($files);
    }
  }

}
