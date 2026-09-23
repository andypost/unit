<?php

declare(strict_types=1);

namespace Drupal\freeunit\StaticCache;

use Drupal\Core\Database\Connection;

/**
 * Which cache tags each static cache file depends on.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.
 *
 * Table freeunit_static_cache (freeunit.install): one row per (file, tag),
 * plus the file's expiry (0 = until invalidated), mirroring how page_cache
 * stores the response in cache.page with the response's cache tags.
 */
final class StaticCacheIndex {

  public const TABLE = 'freeunit_static_cache';

  public function __construct(
    private readonly Connection $database,
  ) {}

  /**
   * Records a file and its tags.  Called before the file is renamed into
   * place, so that an invalidation that races the write always finds it.
   *
   * @param string[] $tags
   */
  public function record(string $file, array $tags, int $expire): void {
    $transaction = $this->database->startTransaction();
    $this->database->delete(self::TABLE)->condition('file', $file)->execute();
    $insert = $this->database->insert(self::TABLE)->fields(['file', 'tag', 'expire']);
    // Every file gets the pseudo-tag "freeunit:all", so a purge is one query.
    foreach (array_unique([...$tags, 'freeunit:all']) as $tag) {
      $insert->values(['file' => $file, 'tag' => $tag, 'expire' => $expire]);
    }
    $insert->execute();
    unset($transaction);
  }

  /**
   * The files that depend on any of these tags.
   *
   * @param string[] $tags
   *
   * @return string[]
   */
  public function filesForTags(array $tags): array {
    if ($tags === []) {
      return [];
    }
    return $this->database->select(self::TABLE, 'f')
      ->fields('f', ['file'])
      ->condition('tag', $tags, 'IN')
      ->distinct()
      ->execute()
      ->fetchCol();
  }

  /**
   * The files whose expiry has passed.
   *
   * @return string[]
   */
  public function expiredFiles(int $now): array {
    return $this->database->select(self::TABLE, 'f')
      ->fields('f', ['file'])
      ->condition('expire', 0, '>')
      ->condition('expire', $now, '<=')
      ->distinct()
      ->execute()
      ->fetchCol();
  }

  /**
   * @param string[] $files
   */
  public function forget(array $files): void {
    foreach (array_chunk($files, 500) as $chunk) {
      $this->database->delete(self::TABLE)->condition('file', $chunk, 'IN')->execute();
    }
  }

  public function forgetAll(): void {
    $this->database->truncate(self::TABLE)->execute();
  }

}
