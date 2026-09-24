<?php

declare(strict_types=1);

namespace Drupal\freeunit\StaticCache;

use Drupal\Core\Config\ConfigFactoryInterface;
use Drupal\Core\Session\SessionConfigurationInterface;
use Symfony\Component\HttpFoundation\Request;

/**
 * Generates the FreeUnit "drupal_page" route for the static page cache.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.  Its output has the shape
 * of routes.drupal_page in examples/unit-drupal-freeunit.json, which was
 * PUT to a local unitd and exercised by examples/check-static-cache.py.
 *
 * Everything comes from freeunit.settings, so the route and the writer
 * (StaticCacheWriter) agree on hosts, scheme, and the headers a cached page
 * carries.
 *
 * Rule order matters, and was chosen from observed router behaviour:
 * 1. Session bypass by exact cookie name.  "cookies" parses every Cookie
 *    header line (src/nxt_http_route.c:2073), so it also catches a session
 *    cookie on a second Cookie line.
 * 2. Session bypass by "headers": {"Cookie": "*SESS*"}, for session names
 *    this generator did not foresee.  On its own it is not enough: every
 *    Cookie line must match (src/nxt_http_route.c:1962-1990).
 * 3. Authorization bypass: page_cache's request policy ignores such
 *    requests (basic_auth), so must the router.
 * 4./5. The gzip and identity shares.  A negated match cannot express
 *    "no session cookie": a header or cookie that is absent never matches,
 *    even "!*" (same function), so bypasses must come first as positive
 *    rules.
 * 6. Everything else goes to PHP.
 */
final class RouteConfigGenerator {

  public function __construct(
    private readonly ConfigFactoryInterface $configFactory,
    private readonly SessionConfigurationInterface $sessionConfiguration,
    private readonly StaticCachePathMapper $mapper,
  ) {}

  /**
   * @return list<array<string, mixed>>
   */
  public function build(): array {
    $settings = $this->configFactory->get('freeunit.settings');
    $app = 'applications/' . $settings->get('control.application');
    if ((string) $settings->get('control.target') !== '') {
      $app .= '/' . $settings->get('control.target');
    }
    $hosts = (array) $settings->get('static_cache.hosts');
    $scheme = (string) $settings->get('static_cache.scheme');
    $gzip = (bool) $settings->get('static_cache.gzip');

    $cookies = [];
    foreach ($hosts as $host) {
      foreach (['http', 'https'] as $s) {
        $options = $this->sessionConfiguration->getOptions(Request::create("$s://$host/"));
        $cookies[$options['name']] = TRUE;
      }
    }

    $routes = [
      [
        'match' => ['cookies' => array_map(fn($n) => [$n => '*'], array_keys($cookies))],
        'action' => ['pass' => $app],
      ],
      [
        'match' => ['headers' => ['Cookie' => '*SESS*']],
        'action' => ['pass' => $app],
      ],
      [
        'match' => ['headers' => ['Authorization' => '*']],
        'action' => ['pass' => $app],
      ],
    ];

    $headers = ['X-Drupal-Cache' => 'HIT-FREEUNIT'];
    foreach ((array) $settings->get('static_cache.headers') as $name => $value) {
      $headers[ucwords((string) $name, '-')] = (string) $value;
    }
    if ($gzip) {
      // The route picks the variant by Accept-Encoding, so say so.
      $headers['Vary'] = trim(($headers['Vary'] ?? '') . ', Accept-Encoding', ', ');
    }

    foreach ($hosts as $host) {
      $dir = $this->mapper->hostDirectory($host);
      if ($dir === NULL) {
        continue;
      }
      $match = [
        'method' => ['GET', 'HEAD'],
        'host' => $host,
        'scheme' => $scheme,
        'query' => '',
      ];
      $action = [
        'share' => $dir . '${uri}' . StaticCachePathMapper::SUFFIX,
        'chroot' => $dir . '/',
        'follow_symlinks' => FALSE,
        'response_headers' => $headers,
        'fallback' => ['pass' => $app],
      ];
      if ($gzip) {
        $routes[] = [
          'match' => $match + ['headers' => ['Accept-Encoding' => '*gzip*']],
          'action' => [
            'share' => $action['share'] . '.gz',
            'response_headers' => $headers + ['Content-Encoding' => 'gzip'],
          ] + $action,
        ];
      }
      $routes[] = ['match' => $match, 'action' => $action];
    }

    $routes[] = ['action' => ['pass' => $app]];
    return $routes;
  }

}
