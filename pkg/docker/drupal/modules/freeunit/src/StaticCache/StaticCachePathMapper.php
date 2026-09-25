<?php

declare(strict_types=1);

namespace Drupal\freeunit\StaticCache;

use Drupal\Core\Config\ConfigFactoryInterface;
use Symfony\Component\HttpFoundation\Request;

/**
 * Maps a request to the file the FreeUnit router serves for it.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.
 *
 * The router side is a "share" of
 * "<directory>/<scheme>/<host>${uri}_.html" (see
 * examples/unit-drupal-freeunit.json).  $uri is FreeUnit's decoded,
 * dot-segment-normalised path, so the mapping here only accepts paths for
 * which the raw request path and that normalised form are byte-identical:
 * a conservative ASCII subset with no percent-encoding, no empty segment and
 * no dot segment.  Anything else is simply not cached, and keeps working
 * through PHP.
 *
 * Appending "_.html" makes the mapping injective and keeps pages from
 * colliding with directories: "/node" is "node_.html", "/node/" is
 * "node/_.html", "/node/1" is "node/1_.html".
 */
final class StaticCachePathMapper {

  public const SUFFIX = '_.html';

  public function __construct(
    private readonly ConfigFactoryInterface $configFactory,
  ) {}

  /**
   * The cache root, without a trailing slash.
   */
  public function root(): string {
    $dir = (string) $this->configFactory->get('freeunit.settings')->get('static_cache.directory');
    return rtrim($dir, '/');
  }

  /**
   * The directory for one host, or NULL if the host is unusable.
   *
   * The scheme is the configured one (what the router sees), not the
   * request's: behind a TLS-terminating proxy Drupal says "https" while the
   * route matches "http", and both sides must agree on the tree.
   */
  public function hostDirectory(string $host): ?string {
    $settings = $this->configFactory->get('freeunit.settings');
    $scheme = (string) $settings->get('static_cache.scheme');
    $host = strtolower($host);
    // Host names and IPv4 literals only; the port is not part of the key
    // because the route matches on "host", which excludes the port.
    if (!in_array($scheme, ['http', 'https'], TRUE)
      || !preg_match('/^[a-z0-9]([a-z0-9-]{0,62})(\.[a-z0-9]([a-z0-9-]{0,62}))*$/', $host)) {
      return NULL;
    }
    return $this->root() . '/' . $scheme . '/' . $host;
  }

  /**
   * The file for a request, or NULL when the request is not mappable.
   */
  public function fileFor(Request $request): ?string {
    if ($request->getQueryString() !== NULL && $request->getQueryString() !== '') {
      return NULL;
    }
    // A query-less "?" is fine for the route ("query": "" matches it), but
    // Symfony reports it as no query string, so nothing to do here.
    $path = parse_url($request->getRequestUri(), PHP_URL_PATH);
    if (!is_string($path) || !$this->isSafePath($path)) {
      return NULL;
    }
    // Only hosts the FreeUnit route serves: a file written for any other
    // Host header would never be read, and would let clients fill the disk.
    $host = strtolower($request->getHost());
    $hosts = (array) $this->configFactory->get('freeunit.settings')->get('static_cache.hosts');
    if (!in_array($host, array_map('strtolower', $hosts), TRUE)) {
      return NULL;
    }
    $dir = $this->hostDirectory($host);
    if ($dir === NULL) {
      return NULL;
    }
    return $dir . $path . self::SUFFIX;
  }

  /**
   * Whether FreeUnit's $uri for this raw path is the path itself.
   */
  public function isSafePath(string $path): bool {
    // 512: the index stores the absolute file name in a 1024-byte column.
    if ($path === '' || $path[0] !== '/' || strlen($path) > 512) {
      return FALSE;
    }
    // RFC 3986 pchar minus "%" (no decoding differences), plus "/".
    if (!preg_match('#^[A-Za-z0-9._~!$&\'()*+,;=:@/-]+$#', $path)) {
      return FALSE;
    }
    if (str_contains($path, '//')) {
      return FALSE;
    }
    foreach (explode('/', $path) as $segment) {
      if ($segment === '.' || $segment === '..') {
        return FALSE;
      }
    }
    return TRUE;
  }

}
