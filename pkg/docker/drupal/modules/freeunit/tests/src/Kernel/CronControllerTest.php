<?php

declare(strict_types=1);

namespace Drupal\Tests\freeunit\Kernel;

use Drupal\Core\CronInterface;
use Drupal\KernelTests\KernelTestBase;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\Attributes\RunTestsInSeparateProcesses;
use Symfony\Component\ErrorHandler\BufferingLogger;
use Symfony\Component\HttpFoundation\Request;

/**
 * The cron route: keyed loopback requests only, and the key is never logged.
 */
#[Group('freeunit')]
#[RunTestsInSeparateProcesses]
final class CronControllerTest extends KernelTestBase {

  protected static $modules = ['system'];

  public function testCron(): void {
    $this->container->get('module_installer')->install(['freeunit']);
    $key = 'k3y-' . $this->randomMachineName(32);
    \Drupal::state()->set('system.cron_key', $key);
    $cron = new class() implements CronInterface {

      public int $runs = 0;

      public function run() {
        $this->runs++;
        return TRUE;
      }

    };
    $this->container->set('cron', $cron);
    $logger = new BufferingLogger();
    $this->container->get('logger.factory')->addLogger($logger);

    $get = function (array $server): int {
      $request = Request::create('http://example.org/freeunit/cron', 'GET', [], [], [], $server);
      return $this->container->get('http_kernel')->handle($request)->getStatusCode();
    };
    $this->assertSame(404, $get([]));
    $this->assertSame(404, $get(['HTTP_X_FREEUNIT_CRON_KEY' => 'wrong']));
    $this->assertSame(404, $get(['HTTP_X_FREEUNIT_CRON_KEY' => substr($key, 0, -1)]));
    $this->assertSame(404, $get(['HTTP_X_FREEUNIT_CRON_KEY' => $key, 'REMOTE_ADDR' => '192.0.2.1']));
    $this->assertSame(0, $cron->runs);
    $this->assertSame(204, $get(['HTTP_X_FREEUNIT_CRON_KEY' => $key]));
    $this->assertSame(204, $get(['HTTP_X_FREEUNIT_CRON_KEY' => $key, 'REMOTE_ADDR' => '::1']));
    $this->assertSame(2, $cron->runs);

    $logs = $logger->cleanLogs();
    $this->assertNotEmpty($logs, 'The 404s were logged.');
    array_walk_recursive($logs, function ($v) use ($key) {
      $text = $v instanceof \Throwable ? $v->getMessage() . $v->getTraceAsString() : (is_scalar($v) || $v instanceof \Stringable ? (string) $v : '');
      $this->assertStringNotContainsString($key, $text);
    });

    // Constant-time comparison, and no other comparison of the key.
    $source = file_get_contents(__DIR__ . '/../../../src/Controller/CronController.php');
    $this->assertMatchesRegularExpression('/hash_equals\(\$key, \$given\)/', $source);
    $this->assertDoesNotMatchRegularExpression('/\$given\s*[!=]==?|[!=]==?\s*\$given/', $source);
  }

}
