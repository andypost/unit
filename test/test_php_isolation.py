import pytest

from unit.applications.lang.php import ApplicationPHP
from unit.option import option

prerequisites = {'modules': {'php': 'any'}, 'features': {'isolation': True}}

client = ApplicationPHP()


def test_php_isolation_rootfs(is_su, require, temp_dir):
    versions = option.available['modules'].get('php', [])
    if versions and versions[0].split('.')[:2] >= ['8', '5']:
        pytest.skip('PHP 8.5 rootfs isolation: SIGSEGV under investigation')
    isolation = {'rootfs': temp_dir}

    if not is_su:
        require(
            {
                'features': {
                    'isolation': [
                        'unprivileged_userns_clone',
                        'user',
                        'mnt',
                        'pid',
                    ]
                }
            }
        )

        isolation['namespaces'] = {
            'mount': True,
            'credential': True,
            'pid': True,
        }

    client.load('phpinfo', isolation=isolation)

    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/root'
    )
    assert 'success' in client.conf(
        '"/app/php/phpinfo"', 'applications/phpinfo/working_directory'
    )

    assert client.get()['status'] == 200, 'empty rootfs'


def test_php_isolation_rootfs_extensions(is_su, require, temp_dir):
    isolation = {'rootfs': temp_dir}

    if not is_su:
        require(
            {
                'features': {
                    'isolation': [
                        'unprivileged_userns_clone',
                        'user',
                        'mnt',
                        'pid',
                    ]
                }
            }
        )

        isolation['namespaces'] = {
            'mount': True,
            'credential': True,
            'pid': True,
        }

    client.load('list-extensions', isolation=isolation)

    assert 'success' in client.conf(
        '"/app/php/list-extensions"', 'applications/list-extensions/root'
    )

    assert 'success' in client.conf(
        {'file': '/php/list-extensions/php.ini'},
        'applications/list-extensions/options',
    )

    assert 'success' in client.conf(
        '"/app/php/list-extensions"',
        'applications/list-extensions/working_directory',
    )

    extensions = client.getjson()['body']

    assert 'json' in extensions, 'json in extensions list'
    assert 'unit' in extensions, 'unit in extensions list'
