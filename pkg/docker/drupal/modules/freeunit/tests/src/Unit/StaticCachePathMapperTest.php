<?php

declare(strict_types=1);

namespace Drupal\Tests\freeunit\Unit;

use Drupal\freeunit\StaticCache\StaticCachePathMapper;
use Drupal\Tests\UnitTestCase;
use PHPUnit\Framework\Attributes\DataProvider;
use PHPUnit\Framework\Attributes\Group;
use Symfony\Component\HttpFoundation\Request;

/**
 * Request to file mapping.
 */
#[Group('freeunit')]
final class StaticCachePathMapperTest extends UnitTestCase {

  private function mapper(string $scheme = 'http'): StaticCachePathMapper {
    return new StaticCachePathMapper($this->getConfigFactoryStub([
      'freeunit.settings' => [
        'static_cache' => [
          'directory' => '/cache/',
          'hosts' => ['Example.org'],
          'scheme' => $scheme,
        ],
      ],
    ]));
  }

  public static function uris(): array {
    return [
      'front' => ['http://example.org/', '/cache/http/example.org/_.html'],
      'page' => ['http://example.org/node', '/cache/http/example.org/node_.html'],
      'trailing slash' => ['http://example.org/node/', '/cache/http/example.org/node/_.html'],
      'nested' => ['http://example.org/node/1', '/cache/http/example.org/node/1_.html'],
      'https request, http tree' => ['https://example.org/a', '/cache/http/example.org/a_.html'],
      'port ignored' => ['http://example.org:8080/a', '/cache/http/example.org/a_.html'],
      'host case' => ['http://EXAMPLE.org/a', '/cache/http/example.org/a_.html'],
      'sub-delims' => ["http://example.org/a;b=c,d@e:f!\$'()*+~", "/cache/http/example.org/a;b=c,d@e:f!\$'()*+~_.html"],
      'query' => ['http://example.org/a?b=1', NULL],
      'other host' => ['http://evil.example/a', NULL],
      'dot segment' => ['http://example.org/a/./b', NULL],
      'dot-dot segment' => ['http://example.org/a/../b', NULL],
      'trailing dot-dot' => ['http://example.org/a/..', NULL],
      'empty segment' => ['http://example.org/a//b', NULL],
      'percent encoding' => ['http://example.org/a%2Fb', NULL],
      'encoded dot' => ['http://example.org/%2e%2e/b', NULL],
      'non-ASCII' => ['http://example.org/caf%C3%A9', NULL],
      'too long' => ['http://example.org/' . str_repeat('a', 512), NULL],
    ];
  }

  #[DataProvider('uris')]
  public function testFileFor(string $uri, ?string $expected): void {
    $this->assertSame($expected, $this->mapper()->fileFor(Request::create($uri)));
  }

  public function testHostDirectory(): void {
    $this->assertSame('/cache/https/example.org', $this->mapper('https')->hostDirectory('example.org'));
    $this->assertSame('/cache/http/127.0.0.1', $this->mapper()->hostDirectory('127.0.0.1'));
    $this->assertNull($this->mapper('ftp')->hostDirectory('example.org'));
    $this->assertNull($this->mapper()->hostDirectory('a/b'));
    $this->assertNull($this->mapper()->hostDirectory('-a.org'));
    $this->assertNull($this->mapper()->hostDirectory('[::1]'));
  }

}
