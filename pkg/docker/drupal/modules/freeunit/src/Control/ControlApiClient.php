<?php

declare(strict_types=1);

namespace Drupal\freeunit\Control;

use Drupal\Core\Config\ConfigFactoryInterface;

/**
 * A minimal client for the FreeUnit control API over its Unix socket.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.  The request shapes were
 * checked against a local unitd (see examples/check-static-cache.py and
 * docs/drupal/freeunit-module.md).
 *
 * Use it from Drush, as the deploy user, not from web requests: the socket
 * is 0600 root by default (unitd --control-mode, src/nxt_runtime.c:1072),
 * and a group-writable socket would let any PHP code rewrite the server
 * configuration, including the user applications run as.
 *
 * The control API has no PATCH (src/nxt_controller.c:1427-1583: GET, POST,
 * PUT, DELETE), and a PUT to /config/a/b needs /config/a to exist; putPath()
 * creates the parent when needed.
 */
final class ControlApiClient {

  public function __construct(
    private readonly string $socket,
    private readonly float $timeout = 10.0,
  ) {}

  public static function fromConfig(ConfigFactoryInterface $configFactory): self {
    return new self((string) $configFactory->get('freeunit.settings')->get('control.socket'));
  }

  public function isAvailable(): bool {
    return is_writable($this->socket);
  }

  /**
   * GET /config/<path> or /status/<path>; NULL on 404.
   */
  public function get(string $path): mixed {
    [$status, $body] = $this->request('GET', $path);
    if ($status === 404) {
      return NULL;
    }
    $this->assertOk($status, $body, 'GET', $path);
    return json_decode($body, TRUE, 512, JSON_THROW_ON_ERROR);
  }

  /**
   * PUT a JSON value, creating the parent object when it does not exist.
   */
  public function putPath(string $path, mixed $value): void {
    $json = json_encode($value, JSON_THROW_ON_ERROR | JSON_UNESCAPED_SLASHES);
    [$status, $body] = $this->request('PUT', $path, $json);
    if ($status === 404 && preg_match('#^(/config/.+)/([^/]+)$#', $path, $m)) {
      $parent = $this->get($m[1]);
      if ($parent === NULL) {
        $this->putPath($m[1], [rawurldecode($m[2]) => $value]);
        return;
      }
    }
    $this->assertOk($status, $body, 'PUT', $path);
  }

  public function delete(string $path): void {
    [$status, $body] = $this->request('DELETE', $path);
    if ($status !== 404) {
      $this->assertOk($status, $body, 'DELETE', $path);
    }
  }

  /**
   * Graceful restart: a new prototype and workers; running requests finish.
   *
   * See src/nxt_controller.c:2470 (GET only), src/nxt_router.c:1602.  A new
   * prototype re-runs PHP module startup, so OPcache starts empty and
   * opcache.preload runs again.
   */
  public function restartApplication(string $name): void {
    $path = '/control/applications/' . rawurlencode($name) . '/restart';
    [$status, $body] = $this->request('GET', $path);
    $this->assertOk($status, $body, 'GET', $path);
  }

  /**
   * @return array{0: int, 1: string}
   */
  private function request(string $method, string $path, ?string $body = NULL): array {
    $errno = 0;
    $errstr = '';
    $conn = @stream_socket_client('unix://' . $this->socket, $errno, $errstr, $this->timeout);
    if ($conn === FALSE) {
      throw new ControlApiException(sprintf('Cannot connect to %s: %s', $this->socket, $errstr));
    }
    stream_set_timeout($conn, (int) ceil($this->timeout));
    $req = sprintf("%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n", $method, $path);
    if ($body !== NULL) {
      $req .= "Content-Type: application/json\r\nContent-Length: " . strlen($body) . "\r\n";
    }
    fwrite($conn, $req . "\r\n" . ($body ?? ''));
    $raw = stream_get_contents($conn);
    fclose($conn);
    if (!is_string($raw) || !preg_match('#^HTTP/1\.[01] (\d{3})#', $raw, $m)) {
      throw new ControlApiException(sprintf('No valid response for %s %s.', $method, $path));
    }
    $parts = explode("\r\n\r\n", $raw, 2);
    return [(int) $m[1], $parts[1] ?? ''];
  }

  private function assertOk(int $status, string $body, string $method, string $path): void {
    if ($status < 200 || $status > 299) {
      throw new ControlApiException(sprintf('%s %s: HTTP %d: %s', $method, $path, $status, trim($body)));
    }
  }

}
