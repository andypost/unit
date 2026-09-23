<?php

declare(strict_types=1);

namespace Drupal\freeunit\StaticCache;

use Psr\Log\LoggerInterface;

/**
 * File operations on the static cache directory.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.
 *
 * The FreeUnit router reads these files as the router's user, not as the
 * PHP worker's user, so files are 0644 and directories 0755.  Every write is
 * a rename() of a complete temporary file in the same directory: the router
 * opens either the old file or the new one, never a partial one.
 */
final class StaticCacheFiles {

  public function __construct(
    private readonly StaticCachePathMapper $mapper,
    private readonly LoggerInterface $logger,
  ) {}

  /**
   * Writes the page and, optionally, its gzip variant.
   */
  public function write(string $file, string $html, bool $gzip): bool {
    if (!$this->isUnderRoot($file)) {
      return FALSE;
    }
    $dir = dirname($file);
    if (!is_dir($dir) && !@mkdir($dir, 0755, TRUE) && !is_dir($dir)) {
      $this->logger->warning('Cannot create @dir.', ['@dir' => $dir]);
      return FALSE;
    }
    // The .gz goes first: the gzip route falls back to PHP when it is
    // missing, and a .gz that is newer than the .html is never served stale
    // relative to it.
    if ($gzip) {
      $encoded = gzencode($html, 6);
      if ($encoded === FALSE || !$this->atomicWrite($file . '.gz', $encoded)) {
        return FALSE;
      }
    }
    return $this->atomicWrite($file, $html);
  }

  /**
   * Deletes the page and its variants.
   *
   * @param string[] $files
   */
  public function delete(array $files): void {
    foreach ($files as $file) {
      if (!$this->isUnderRoot($file)) {
        continue;
      }
      @unlink($file . '.gz');
      @unlink($file);
    }
  }

  /**
   * Removes everything: renames the root aside, then deletes it.
   *
   * The rename is atomic, so the router sees either the full old cache or an
   * empty one, and a writer racing the purge writes into a fresh tree.
   */
  public function purge(): void {
    $root = $this->mapper->root();
    if ($root === '' || $root === '/' || !is_dir($root)) {
      return;
    }
    $trash = $root . '.purge-' . bin2hex(random_bytes(4));
    if (!@rename($root, $trash)) {
      $this->logger->warning('Cannot rename @root for a purge.', ['@root' => $root]);
      return;
    }
    @mkdir($root, 0755, TRUE);
    $this->removeTree($trash);
  }

  private function atomicWrite(string $file, string $data): bool {
    $tmp = @tempnam(dirname($file), '.freeunit-');
    if ($tmp === FALSE) {
      return FALSE;
    }
    if (@file_put_contents($tmp, $data) !== strlen($data)
      || !@chmod($tmp, 0644)
      || !@rename($tmp, $file)) {
      @unlink($tmp);
      return FALSE;
    }
    return TRUE;
  }

  private function isUnderRoot(string $file): bool {
    $root = $this->mapper->root();
    return $root !== '' && str_starts_with($file, $root . '/') && !str_contains($file, '/../');
  }

  private function removeTree(string $dir): void {
    $it = new \RecursiveIteratorIterator(
      new \RecursiveDirectoryIterator($dir, \FilesystemIterator::SKIP_DOTS),
      \RecursiveIteratorIterator::CHILD_FIRST,
    );
    foreach ($it as $entry) {
      $entry->isDir() && !$entry->isLink() ? @rmdir($entry->getPathname()) : @unlink($entry->getPathname());
    }
    @rmdir($dir);
  }

}
