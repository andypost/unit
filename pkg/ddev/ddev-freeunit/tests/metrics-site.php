<?php

/**
 * @file
 * Drupal chores for metrics.sh without drush (drush does not install on
 * Drupal 12 pre-releases yet). Run in the web container from the project
 * root:  php metrics-site.php install|node|version|log|module-on <m>|module-off <m>
 */

use Drupal\Core\DrupalKernel;
use Drupal\field\Entity\FieldConfig;
use Drupal\field\Entity\FieldStorageConfig;
use Drupal\node\Entity\NodeType;
use Drupal\user\Entity\Role;
use Symfony\Component\HttpFoundation\Request;

$host = getenv('DDEV_HOSTNAME') ?: 'localhost';
$_SERVER += [
  'HTTP_HOST' => $host,
  'SERVER_NAME' => $host,
  'SERVER_PORT' => 80,
  'REMOTE_ADDR' => '127.0.0.1',
  'REQUEST_METHOD' => 'GET',
  'REQUEST_URI' => '/',
  'SCRIPT_NAME' => '/index.php',
  'SCRIPT_FILENAME' => '/var/www/html/web/index.php',
  'SERVER_SOFTWARE' => NULL,
];
chdir('/var/www/html/web');
$loader = require 'autoload.php';
$cmd = $argv[1] ?? '';

if ($cmd === 'install') {
  // The database comes from DDEV's settings.ddev.php, as for `drush si`.
  require_once 'core/includes/install.core.inc';
  install_drupal($loader, [
    'interactive' => FALSE,
    'parameters' => ['profile' => 'standard', 'langcode' => 'en'],
    'forms' => [
      'install_configure_form' => [
        'site_name' => 'bench',
        'site_mail' => 'admin@example.com',
        'account' => [
          'name' => 'admin',
          'mail' => 'admin@example.com',
          'pass' => ['pass1' => 'admin', 'pass2' => 'admin'],
        ],
        'enable_update_status_module' => FALSE,
        'enable_update_status_emails' => FALSE,
      ],
    ],
  ]);
  echo "installed\n";
  exit;
}

$request = Request::createFromGlobals();
$kernel = DrupalKernel::createFromRequest($request, $loader, 'prod');
$kernel->boot();
$kernel->preHandle($request);
$kernel->getContainer()->get('request_stack')->push($request);

switch ($cmd) {
  case 'node':
    // Standard in Drupal 11.4 and 12 ships no "article" type, and Drupal 12
    // does not grant "access content" to anonymous users: set up both.
    if (!NodeType::load('bench')) {
      NodeType::create(['type' => 'bench', 'name' => 'Bench'])->save();
    }
    if (!NodeType::load('bench')) {
      fwrite(STDERR, "node type 'bench' was not created\n");
      exit(1);
    }
    if (FieldStorageConfig::loadByName('node', 'body') && !FieldConfig::loadByName('node', 'bench', 'body')) {
      FieldConfig::create(['entity_type' => 'node', 'bundle' => 'bench', 'field_name' => 'body', 'label' => 'Body'])->save();
      \Drupal::service('entity_display.repository')->getViewDisplay('node', 'bench')
        ->setComponent('body', ['type' => 'text_default', 'label' => 'hidden'])->save();
    }
    foreach (['anonymous', 'authenticated'] as $rid) {
      Role::load($rid)->grantPermission('access content')->save();
    }
    $values = ['type' => 'bench', 'title' => 'Bench', 'uid' => 1, 'status' => 1, 'promote' => 1];
    $node = \Drupal::entityTypeManager()->getStorage('node')->create($values);
    if ($node->hasField('body')) {
      $node->set('body', [
        'value' => str_repeat('<p>Lorem ipsum dolor sit amet.</p>', 50),
        'format' => 'plain_text',
      ]);
    }
    $node->save();
    // Without the frontpage view, "/" would have nothing to show.
    $views = \Drupal::entityTypeManager()->hasDefinition('view')
      ? \Drupal::entityTypeManager()->getStorage('view')->load('frontpage') : NULL;
    if (!$views || !$views->status()) {
      \Drupal::configFactory()->getEditable('system.site')->set('page.front', '/node/' . $node->id())->save();
    }
    echo $node->id(), "\n";
    break;

  case 'version':
    echo 'Drupal ', \Drupal::VERSION, ', PHP ', PHP_VERSION, "\n";
    break;

  case 'module-on':
    \Drupal::service('module_installer')->install([$argv[2]]);
    break;

  case 'module-off':
    \Drupal::service('module_installer')->uninstall([$argv[2]]);
    break;

  case 'log':
    // The last errors from dblog, to explain an unexpected status code.
    $rows = \Drupal::database()->select('watchdog', 'w')
      ->fields('w', ['type', 'message', 'variables', 'location'])
      ->condition('severity', 3, '<=')
      ->orderBy('wid', 'DESC')
      ->range(0, 5)
      ->execute();
    foreach ($rows as $r) {
      $vars = @unserialize($r->variables, ['allowed_classes' => FALSE]) ?: [];
      echo "[{$r->type}] {$r->location}: ", strip_tags(strtr($r->message, array_map('strval', array_filter($vars, 'is_scalar')))), "\n";
    }
    break;

  default:
    fwrite(STDERR, "unknown command '$cmd'\n");
    exit(1);
}
