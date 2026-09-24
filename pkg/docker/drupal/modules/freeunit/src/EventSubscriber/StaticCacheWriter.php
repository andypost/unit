<?php

declare(strict_types=1);

namespace Drupal\freeunit\EventSubscriber;

use Drupal\Core\Cache\CacheableResponseInterface;
use Drupal\Core\Cache\CacheBackendInterface;
use Drupal\Core\Config\ConfigFactoryInterface;
use Drupal\Core\Session\AccountInterface;
use Drupal\Core\Session\SessionConfigurationInterface;
use Drupal\freeunit\StaticCache\StaticCacheFiles;
use Drupal\freeunit\StaticCache\StaticCacheIndex;
use Drupal\freeunit\StaticCache\StaticCachePathMapper;
use Psr\Log\LoggerInterface;
use Symfony\Component\EventDispatcher\EventSubscriberInterface;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;
use Symfony\Component\HttpKernel\Event\TerminateEvent;
use Symfony\Component\HttpKernel\KernelEvents;

/**
 * Writes anonymous page-cache responses to files for the FreeUnit router.
 *
 * UNVERIFIED PROTOTYPE: never run inside Drupal.
 *
 * Runs on kernel.terminate.  Drupal's index.php calls $response->send() and
 * then $kernel->terminate(); Symfony's Response::send() calls
 * fastcgi_finish_request() when it exists, and FreeUnit's PHP SAPI provides
 * it (src/nxt_php_sapi.c:228) and reports the rest of the script to the
 * router as detached work (src/nxt_php_sapi.c:279).  So the file write costs
 * the visitor nothing.
 *
 * The rule is "write exactly what page_cache holds, and nothing else":
 * X-Drupal-Cache is MISS only after PageCache::storeResponse() succeeded and
 * HIT only for a response PageCache just fetched, and the write is then
 * cross-checked against the cache.page item, so a tag invalidation that
 * raced the render wins.
 */
final class StaticCacheWriter implements EventSubscriberInterface {

  /**
   * Response headers the router does not reproduce and that may be dropped.
   *
   * The router sends its own Date, ETag and Last-Modified; the others are
   * Drupal debugging or informational headers.
   */
  private const DROPPED = [
    'date',
    'etag',
    'last-modified',
    'link',
    'x-drupal-cache',
    'x-drupal-cache-tags',
    'x-drupal-cache-contexts',
    'x-drupal-cache-max-age',
    'x-drupal-dynamic-cache',
    'x-generator',
    'x-ua-compatible',
  ];

  public function __construct(
    private readonly ConfigFactoryInterface $configFactory,
    private readonly AccountInterface $currentUser,
    private readonly SessionConfigurationInterface $sessionConfiguration,
    private readonly CacheBackendInterface $pageCache,
    private readonly StaticCachePathMapper $mapper,
    private readonly StaticCacheFiles $files,
    private readonly StaticCacheIndex $index,
    private readonly LoggerInterface $logger,
  ) {}

  public static function getSubscribedEvents(): array {
    // After the invalidator's second pass (-50) and automated_cron (100).
    return [KernelEvents::TERMINATE => [['onTerminate', -100]]];
  }

  public function onTerminate(TerminateEvent $event): void {
    $settings = $this->configFactory->get('freeunit.settings');
    if (!$settings->get('static_cache.enabled')) {
      return;
    }
    $request = $event->getRequest();
    $response = $event->getResponse();

    $file = $this->mapper->fileFor($request);
    if ($file === NULL || !$this->isWritable($request, $response, $settings->get('static_cache'))) {
      return;
    }
    // A HIT is written only when the file is missing (after a purge, an
    // expiry, or enabling the module on a warm site); a MISS always.
    if ($response->headers->get('X-Drupal-Cache') === 'HIT' && file_exists($file)) {
      return;
    }
    $cid = $this->findPageCacheItem($request);
    if ($cid === NULL) {
      // page_cache did not keep it (or it was invalidated already).
      return;
    }

    /** @var \Drupal\Core\Cache\CacheableResponseInterface $response */
    $tags = $response->getCacheableMetadata()->getCacheTags();
    $expire = 0;
    if (($expires = $response->getExpires()) !== NULL && $expires->getTimestamp() > time()) {
      // Mirrors PageCache::storeResponse(): a future Expires bounds the entry.
      // The 1978 Expires that Drupal sends with "Vary: Cookie" is in the past
      // and means "until invalidated", as it does for page_cache.
      $expire = $expires->getTimestamp();
    }

    // Index first, file second: an invalidation from here on finds the file.
    $this->index->record($file, $tags, $expire);
    if (!$this->files->write($file, (string) $response->getContent(), (bool) $settings->get('static_cache.gzip'))) {
      $this->index->forget([$file]);
      return;
    }

    // Re-check: if the tags were invalidated while this page was rendered or
    // written, the cache.page item is gone or invalid now; drop the file.
    if ($this->pageCache->get($cid) === FALSE) {
      $this->files->delete([$file]);
      $this->index->forget([$file]);
    }
  }

  /**
   * The conditions under which the response is a faithful anonymous page.
   *
   * @param array<string, mixed> $conf
   */
  private function isWritable(Request $request, Response $response, array $conf): bool {
    if ($request->getMethod() !== 'GET'
      || !$this->currentUser->isAnonymous()
      || $this->sessionConfiguration->hasSession($request)
      || $response->getStatusCode() !== 200
      || !$response instanceof CacheableResponseInterface
      // A BigPipe response's getContent() holds placeholders, not the
      // streamed parts; never write one.  Class name only: big_pipe may be
      // off.
      || is_a($response, 'Drupal\big_pipe\Render\BigPipeResponse')
      || !in_array($response->headers->get('X-Drupal-Cache'), ['MISS', 'HIT'], TRUE)
      || $response->headers->getCookies() !== []
      // Defence in depth on top of page_cache's own response policy.
      || !$response->headers->hasCacheControlDirective('public')
      || $response->headers->hasCacheControlDirective('private')
      || $response->headers->hasCacheControlDirective('no-cache')
      || $response->headers->hasCacheControlDirective('no-store')) {
      return FALSE;
    }
    $content = $response->getContent();
    if (!is_string($content) || $content === '' || strlen($content) > (int) $conf['max_bytes']) {
      return FALSE;
    }
    // Every header must either carry exactly the value the route reproduces
    // for every page, or be one the route may drop; otherwise the router
    // would serve something Drupal did not send (another Content-Language,
    // another Cache-Control, a Vary the route does not know).
    $fixed = array_change_key_case((array) $conf['headers'], CASE_LOWER);
    foreach ($response->headers->allPreserveCaseWithoutCookies() as $name => $values) {
      $name = strtolower((string) $name);
      if (in_array($name, self::DROPPED, TRUE)) {
        continue;
      }
      if (!isset($fixed[$name]) || $values !== [(string) $fixed[$name]]) {
        $this->logger->debug('Not writing @uri: header @h is not what the route reproduces.', [
          '@uri' => $request->getRequestUri(),
          '@h' => $name,
        ]);
        return FALSE;
      }
    }
    return TRUE;
  }

  /**
   * The cache.page cid PageCache used, if its item is still valid.
   *
   * PageCache::getCacheId() is protected and memoised at lookup time as
   * "<scheme+host><request URI>:<request format at lookup>".  For an HTML
   * page the format is usually NULL at lookup, so try that first.
   */
  private function findPageCacheItem(Request $request): ?string {
    $base = $request->getSchemeAndHttpHost() . $request->getRequestUri() . ':';
    foreach (array_unique([$base, $base . $request->getRequestFormat(NULL)]) as $cid) {
      if ($this->pageCache->get($cid) !== FALSE) {
        return $cid;
      }
    }
    return NULL;
  }

}
