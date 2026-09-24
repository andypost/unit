<?php

declare(strict_types=1);

namespace Drupal\Tests\freeunit\Kernel;

use Drupal\Core\Cache\Cache;
use Drupal\Core\Cache\CacheableMetadata;
use Drupal\Core\Extension\Requirement\RequirementSeverity;
use Drupal\Core\Render\HtmlResponse;
use Drupal\KernelTests\KernelTestBase;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpKernel\Event\TerminateEvent;
use Symfony\Component\HttpKernel\KernelEvents;

/**
 * The writer, the tag index, the invalidator, purge and uninstall.
 */
#[Group('freeunit')]
#[RunTestsInSeparateProcesses]
final class StaticCacheTest extends KernelTestBase {

  protected static $modules = ['system'];

  private string $dir;

  private string $file;

  protected function setUp(): void {
    parent::setUp();
    $this->container->get('module_installer')->install(['freeunit']);
    $this->dir = sys_get_temp_dir() . '/freeunit-' . $this->randomMachineName();
    mkdir($this->dir);
    $this->file = $this->dir . '/http/example.org/node/1_.html';
    $this->config('freeunit.settings')
      ->set('static_cache.enabled', TRUE)
      ->set('static_cache.directory', $this->dir)
      ->set('static_cache.hosts', ['example.org'])
      ->save();
  }

  protected function tearDown(): void {
    exec('rm -rf ' . escapeshellarg($this->dir));
    parent::tearDown();
  }

  /**
   * Renders /node/1 as page_cache would have stored it, then terminates.
   */
  private function terminate(bool $in_page_cache = TRUE): void {
    $tags = ['node:1', 'rendered'];
    if ($in_page_cache) {
      \Drupal::cache('page')->set('http://example.org/node/1:', 'x', Cache::PERMANENT, $tags);
    }
    $response = new HtmlResponse('<html>node 1</html>');
    $response->headers->add(array_change_key_case(
      $this->config('freeunit.settings')->get('static_cache.headers'), CASE_LOWER,
    ) + ['x-drupal-cache' => 'MISS']);
    $response->addCacheableDependency((new CacheableMetadata())->setCacheTags($tags));
    $kernel = $this->container->get('http_kernel');
    $this->container->get('event_dispatcher')->dispatch(
      new TerminateEvent($kernel, Request::create('http://example.org/node/1'), $response),
      KernelEvents::TERMINATE,
    );
  }

  private function tags(): array {
    return \Drupal::database()->select('freeunit_static_cache', 'f')
      ->fields('f', ['tag'])->condition('file', $this->file)->orderBy('tag')
      ->execute()->fetchCol();
  }

  public function testWriteAndInvalidate(): void {
    $this->terminate(FALSE);
    $this->assertFileDoesNotExist($this->file, 'Nothing is written that page_cache does not hold.');

    $this->terminate();
    $this->assertSame('<html>node 1</html>', file_get_contents($this->file));
    $this->assertSame('<html>node 1</html>', gzdecode(file_get_contents($this->file . '.gz')));
    $this->assertSame(0644, fileperms($this->file) & 0777);
    $this->assertSame(['freeunit:file', 'node:1', 'rendered'], $this->tags());
    $this->assertSame([], glob($this->dir . '/http/example.org/node/.freeunit-*'), 'No temporary file is left.');

    Cache::invalidateTags(['user:1']);
    $this->assertFileExists($this->file);
    Cache::invalidateTags(['node:1']);
    $this->assertFileDoesNotExist($this->file);
    $this->assertFileDoesNotExist($this->file . '.gz');
    $this->assertSame([], $this->tags());

    // Expiry through hook_cron.
    $this->terminate();
    \Drupal::service('freeunit.static_cache.index')->record($this->file, ['node:1'], time() - 1);
    \Drupal::moduleHandler()->invokeAll('cron');
    $this->assertFileDoesNotExist($this->file);
  }

  public function testPurgeAndUninstall(): void {
    $this->terminate();
    drupal_flush_all_caches();
    $this->assertFileDoesNotExist($this->file);
    $this->assertDirectoryExists($this->dir);
    $this->assertSame(0, (int) \Drupal::database()->select('freeunit_static_cache')->countQuery()->execute()->fetchField());

    $this->terminate();
    $this->assertFileExists($this->file);
    $this->container->get('module_installer')->uninstall(['freeunit']);
    $this->assertFileDoesNotExist($this->file);
    $this->assertFalse(\Drupal::database()->schema()->tableExists('freeunit_static_cache'));
  }

  public function testRuntimeRequirements(): void {
    $requirements = \Drupal::moduleHandler()->invoke('freeunit', 'runtime_requirements');
    $this->assertSame(RequirementSeverity::Warning, $requirements['freeunit_server']['severity']);
    $this->assertSame(RequirementSeverity::OK, $requirements['freeunit_static_cache']['severity']);
    $this->config('freeunit.settings')->set('static_cache.directory', $this->dir . '/missing')->save();
    $requirements = \Drupal::moduleHandler()->invoke('freeunit', 'runtime_requirements');
    $this->assertSame(RequirementSeverity::Error, $requirements['freeunit_static_cache']['severity']);
  }

}
