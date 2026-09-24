<?php

declare(strict_types=1);

namespace Drupal\Tests\freeunit\Functional;

use Drupal\Tests\BrowserTestBase;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;

/**
 * An anonymous page becomes a cache file whose route headers are exact.
 *
 * Runs against whatever serves SIMPLETEST_BASE_URL; CI runs it under
 * FreeUnit, where the file is written after the response was sent.
 */
#[Group('freeunit')]
#[RunTestsInSeparateProcesses]
final class StaticCacheFileTest extends BrowserTestBase {

  protected static $modules = ['freeunit', 'node'];

  protected $defaultTheme = 'stark';

  /**
   * Headers the web server adds, and the ones the route may drop.
   *
   * X-Drupal-Assertion-* (test sites only) are sent with header() by the
   * test error handler, outside the Response the writer checks.
   */
  private const IGNORED = [
    'connection', 'host', 'keep-alive', 'server', 'transfer-encoding', 'x-powered-by',
    'content-length', 'date', 'etag', 'last-modified', 'link', 'x-drupal-cache',
    'x-drupal-cache-tags', 'x-drupal-cache-contexts', 'x-drupal-cache-max-age',
    'x-drupal-dynamic-cache', 'x-generator', 'x-ua-compatible',
  ];

  public function testAnonymousPage(): void {
    $this->drupalCreateContentType(['type' => 'page']);
    $this->drupalCreateNode(['type' => 'page', 'title' => 'Hello FreeUnit']);
    $this->config('system.performance')->set('cache.page.max_age', 300)->save();
    $dir = $this->root . '/' . $this->publicFilesDirectory . '/page-cache';
    $host = parse_url($this->baseUrl, PHP_URL_HOST);
    $this->config('freeunit.settings')
      ->set('static_cache.enabled', TRUE)
      ->set('static_cache.directory', $dir)
      ->set('static_cache.hosts', [$host])
      ->save();
    $path = rtrim((string) parse_url($this->baseUrl, PHP_URL_PATH), '/') . '/node/1';
    $file = "$dir/http/$host$path" . '_.html';

    $this->drupalGet('node/1');
    $this->assertSession()->statusCodeEquals(200);
    $this->assertSession()->responseHeaderEquals('X-Drupal-Cache', 'MISS');
    $body = $this->getSession()->getPage()->getContent();

    // kernel.terminate runs after the response has been sent.
    for ($i = 0; $i < 50 && !file_exists($file); $i++) {
      usleep(100000);
    }
    $this->assertFileExists($file);
    // Mink's page content has lost the trailing newline.
    $this->assertSame($body, rtrim(file_get_contents($file)));
    $this->assertSame($body, rtrim(gzdecode(file_get_contents("$file.gz"))));

    $sent = [];
    foreach ($this->getSession()->getResponseHeaders() as $name => $values) {
      if (!in_array(strtolower($name), self::IGNORED, TRUE) && !str_starts_with(strtolower($name), 'x-drupal-assertion-')) {
        $sent[strtolower($name)] = implode(', ', $values);
      }
    }
    ksort($sent);
    $this->assertSame([
      'cache-control' => 'max-age=300, public',
      'content-language' => 'en',
      'content-type' => 'text/html; charset=utf-8',
      'expires' => 'Sun, 19 Nov 1978 05:00:00 GMT',
      'vary' => 'Cookie',
      'x-content-type-options' => 'nosniff',
      'x-frame-options' => 'SAMEORIGIN',
    ], $sent);
    $this->assertSame($sent, $this->config('freeunit.settings')->get('static_cache.headers'));

    // A HIT with the file present is not rewritten; an edit deletes it.
    $this->drupalGet('node/1');
    $this->assertSession()->responseHeaderEquals('X-Drupal-Cache', 'HIT');
    $this->drupalGet('node/1', ['query' => ['a' => 1]]);
    $this->assertFileDoesNotExist("$dir/http/$host$path?a=1_.html");
    $node = $this->container->get('entity_type.manager')->getStorage('node')->load(1);
    $node->setTitle('Changed')->save();
    $this->assertFileDoesNotExist($file);
  }

}
