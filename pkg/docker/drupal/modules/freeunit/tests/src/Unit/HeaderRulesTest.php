<?php

declare(strict_types=1);

namespace Drupal\Tests\freeunit\Unit;

use Drupal\Core\Cache\CacheBackendInterface;
use Drupal\Core\Database\Connection;
use Drupal\Core\Render\HtmlResponse;
use Drupal\Core\Session\AccountInterface;
use Drupal\Core\Session\SessionConfigurationInterface;
use Drupal\freeunit\EventSubscriber\StaticCacheWriter;
use Drupal\freeunit\StaticCache\RouteConfigGenerator;
use Drupal\freeunit\StaticCache\StaticCacheFiles;
use Drupal\freeunit\StaticCache\StaticCacheIndex;
use Drupal\freeunit\StaticCache\StaticCachePathMapper;
use Drupal\Tests\UnitTestCase;
use PHPUnit\Framework\Attributes\DataProvider;
use PHPUnit\Framework\Attributes\Group;
use Psr\Log\NullLogger;
use Symfony\Component\HttpFoundation\Cookie;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\Yaml\Yaml;

/**
 * The header rules shared by the writer and the route generator.
 */
#[Group('freeunit')]
final class HeaderRulesTest extends UnitTestCase {

  private const MODULE = __DIR__ . '/../../..';

  private function settings(): array {
    $settings = Yaml::parseFile(self::MODULE . '/config/install/freeunit.settings.yml');
    $settings['static_cache']['hosts'] = ['example.org'];
    $settings['static_cache']['directory'] = '/var/lib/drupal-freeunit/page-cache';
    return $settings;
  }

  private function session(): SessionConfigurationInterface {
    $session = $this->createStub(SessionConfigurationInterface::class);
    $session->method('hasSession')->willReturn(FALSE);
    $session->method('getOptions')->willReturnCallback(fn(Request $r) => [
      'name' => ($r->isSecure() ? 'SSESS' : 'SESS') . 'bfabc37432958b063360d3ad6461c9c4',
    ]);
    return $session;
  }

  /**
   * An anonymous page exactly as Drupal 11 sends it with max_age 300.
   */
  private static function drupalResponse(): HtmlResponse {
    $response = new HtmlResponse('<html>page</html>');
    $response->headers->add([
      'Cache-Control' => 'public, max-age=300',
      'Content-language' => 'en',
      'Content-Type' => 'text/html; charset=utf-8',
      'Expires' => 'Sun, 19 Nov 1978 05:00:00 GMT',
      'Vary' => 'Cookie',
      'X-Content-Type-Options' => 'nosniff',
      'X-Frame-Options' => 'SAMEORIGIN',
      'X-Drupal-Cache' => 'MISS',
      'X-Drupal-Dynamic-Cache' => 'UNCACHEABLE',
      'X-Generator' => 'Drupal 11 (https://www.drupal.org)',
      'ETag' => '"1700000000"',
      'Last-Modified' => 'Tue, 14 Nov 2023 22:13:20 GMT',
    ]);
    return $response;
  }

  private static function set(string $h, ?string $v): \Closure {
    return fn(Response $r) => $v === NULL ? $r->headers->remove($h) : $r->headers->set($h, $v);
  }

  public static function responses(): array {
    $set = self::set(...);
    return [
      'as Drupal sends it' => [fn() => NULL, TRUE],
      'page cache HIT' => [$set('X-Drupal-Cache', 'HIT'), TRUE],
      'page cache bypassed' => [$set('X-Drupal-Cache', NULL), FALSE],
      'other language' => [$set('Content-language', 'fr'), FALSE],
      'other max-age' => [$set('Cache-Control', 'public, max-age=60'), FALSE],
      'private' => [$set('Cache-Control', 'private, max-age=300'), FALSE],
      'extra Vary' => [$set('Vary', 'Cookie, Accept-Language'), FALSE],
      'unknown header' => [$set('X-Custom', '1'), FALSE],
      'no Expires' => [$set('Expires', NULL), FALSE],
      'Content-Length dropped' => [$set('Content-Length', '17'), TRUE],
      'cookie' => [fn(Response $r) => $r->headers->setCookie(Cookie::create('a', 'b')), FALSE],
      'not found' => [fn(Response $r) => $r->setStatusCode(404), FALSE],
      'empty' => [fn(Response $r) => $r->setContent(''), FALSE],
      'too large' => [fn(Response $r) => $r->setContent(str_repeat('a', 2097153)), FALSE],
    ];
  }

  #[DataProvider('responses')]
  public function testWriterHeaderRules(\Closure $alter, bool $writable): void {
    $settings = $this->settings();
    $config = $this->getConfigFactoryStub(['freeunit.settings' => $settings]);
    $user = $this->createStub(AccountInterface::class);
    $user->method('isAnonymous')->willReturn(TRUE);
    $mapper = new StaticCachePathMapper($config);
    $writer = new StaticCacheWriter(
      $config,
      $user,
      $this->session(),
      $this->createStub(CacheBackendInterface::class),
      $mapper,
      new StaticCacheFiles($mapper, new NullLogger()),
      new StaticCacheIndex($this->createStub(Connection::class)),
      new NullLogger(),
    );
    $response = self::drupalResponse();
    $alter($response);
    $is_writable = new \ReflectionMethod($writer, 'isWritable');
    $this->assertSame($writable, $is_writable->invoke($writer, Request::create('http://example.org/node/1'), $response, $settings['static_cache']));
  }

  /**
   * The generated route is the one in examples/ (object keys sorted).
   */
  public function testRouteMatchesExample(): void {
    $config = $this->getConfigFactoryStub(['freeunit.settings' => $this->settings()]);
    $generator = new RouteConfigGenerator($config, $this->session(), new StaticCachePathMapper($config));
    $example = json_decode(file_get_contents(self::MODULE . '/examples/unit-drupal-freeunit.json'), TRUE);
    $sort = function (array $a) use (&$sort): array {
      array_is_list($a) || ksort($a);
      return array_map(fn($v) => is_array($v) ? $sort($v) : $v, $a);
    };
    $this->assertSame($sort($example['routes']['drupal_page']), $sort($generator->build()));
  }

}
