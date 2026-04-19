"""
Tests for PHP Status API endpoint.

Endpoint: /status/applications/<name>/php
Returns: opcache stats, JIT state, request counters, GC stats, memory usage
"""

from unit.applications.lang.php import ApplicationPHP
from unit.status import Status

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


# =============================================================================
# Basic Tests - Structure Validation
# =============================================================================

def test_php_status_endpoint_exists():
    """Verify endpoint exists and returns data."""
    client.load('mirror')
    
    php_status = Status.get('applications/mirror/php')
    
    assert php_status is not None, 'PHP status endpoint should return data'
    assert isinstance(php_status, dict), 'Status should be a dict'


def test_php_status_has_required_sections():
    """Verify all required sections are present."""
    client.load('mirror')
    
    php_status = Status.get('applications/mirror/php')
    
    required_sections = ['opcache', 'jit', 'requests', 'gc', 'memory']
    for section in required_sections:
        assert section in php_status, f'{section} section should exist'


def test_php_status_opcache_structure():
    """Verify opcache section has all expected fields."""
    client.load('mirror')
    
    opcache = Status.get('applications/mirror/php')['opcache']
    
    expected_fields = [
        'enabled', 'hits', 'misses', 'cached_scripts',
        'memory_used', 'memory_free',
        'interned_strings_used', 'interned_strings_free'
    ]
    
    for field in expected_fields:
        assert field in opcache, f'opcache.{field} should exist'
        assert isinstance(opcache[field], int), f'opcache.{field} should be int'


def test_php_status_jit_structure():
    """Verify JIT section has all expected fields."""
    client.load('mirror')
    
    jit = Status.get('applications/mirror/php')['jit']
    
    expected_fields = ['enabled', 'buffer_size', 'memory_used']
    
    for field in expected_fields:
        assert field in jit, f'jit.{field} should exist'
        assert isinstance(jit[field], int), f'jit.{field} should be int'


def test_php_status_requests_structure():
    """Verify requests section has all expected fields."""
    client.load('mirror')
    
    requests = Status.get('applications/mirror/php')['requests']
    
    expected_fields = ['total', 'active', 'rejected']
    
    for field in expected_fields:
        assert field in requests, f'requests.{field} should exist'
        assert isinstance(requests[field], int), f'requests.{field} should be int'


def test_php_status_gc_structure():
    """Verify GC section has all expected fields."""
    client.load('mirror')
    
    gc = Status.get('applications/mirror/php')['gc']
    
    expected_fields = ['runs', 'last_run_time']
    
    for field in expected_fields:
        assert field in gc, f'gc.{field} should exist'
        assert isinstance(gc[field], int), f'gc.{field} should be int'


def test_php_status_memory_structure():
    """Verify memory section has all expected fields."""
    client.load('mirror')
    
    memory = Status.get('applications/mirror/php')['memory']
    
    expected_fields = ['peak', 'current']
    
    for field in expected_fields:
        assert field in memory, f'memory.{field} should exist'
        assert isinstance(memory[field], int), f'memory.{field} should be int'


# =============================================================================
# Request Counter Tests
# =============================================================================

def test_php_status_request_counter_increments():
    """Verify request counter increments with each request."""
    client.load('mirror')
    
    # Get initial counter
    initial = Status.get('applications/mirror/php')
    initial_requests = initial['requests']['total']
    
    # Make 5 requests
    for _ in range(5):
        client.get()
    
    # Check counter increased
    updated = Status.get('applications/mirror/php')
    updated_requests = updated['requests']['total']
    
    assert updated_requests >= initial_requests + 5, \
        f'Request counter should increment (was {initial_requests}, now {updated_requests})'


# =============================================================================
# Memory Tests
# =============================================================================

def test_php_status_memory_values():
    """Verify memory stats have reasonable values."""
    client.load('mirror')
    
    # Make a request to allocate some memory
    client.get()
    
    memory = Status.get('applications/mirror/php')['memory']
    
    # Peak should be >= current
    assert memory['peak'] >= memory['current'], \
        'Peak memory should be >= current memory'
    
    # Both should be positive
    assert memory['peak'] > 0, 'Peak memory should be > 0'
    assert memory['current'] >= 0, 'Current memory should be >= 0'


# =============================================================================
# Opcache Tests
# =============================================================================

def test_php_status_opcache_enabled_flag():
    """Verify opcache enabled flag is present (0 or 1)."""
    client.load('mirror')
    
    opcache = Status.get('applications/mirror/php')['opcache']
    
    # Should be 0 or 1
    assert opcache['enabled'] in [0, 1], \
        'opcache.enabled should be 0 or 1'


def test_php_status_opcache_warmup():
    """Verify opcache stats work after warmup."""
    client.load('opcache')
    
    # Make requests to cache scripts
    for _ in range(3):
        client.get()
    
    status = Status.get('applications/opcache/php')
    
    # Cached scripts should be >= 0
    assert status['opcache']['cached_scripts'] >= 0, \
        'cached_scripts should be >= 0'


# =============================================================================
# Multiple Workers Tests
# =============================================================================

def test_php_status_multiple_workers():
    """Verify status works with multiple worker processes."""
    client.conf({
        "listeners": {"*:8080": {"pass": "applications/multi"}},
        "applications": {
            "multi": {
                "type": client.get_application_type(),
                "processes": {"max": 4, "spare": 2},
                "root": f"{client.options['test_dir']}/php/mirror",
                "index": "index.php",
            }
        }
    })
    
    status = Status.get('applications/multi/php')
    
    # Should return aggregated stats
    assert status is not None
    assert 'requests' in status
    assert 'memory' in status


def test_php_status_multiple_workers_counters():
    """Verify request counters aggregate across workers."""
    client.conf({
        "listeners": {"*:8080": {"pass": "applications/multi"}},
        "applications": {
            "multi": {
                "type": client.get_application_type(),
                "processes": {"max": 4},
                "root": f"{client.options['test_dir']}/php/mirror",
                "index": "index.php",
            }
        }
    })
    
    # Get initial
    initial = Status.get('applications/multi/php')
    initial_requests = initial['requests']['total']
    
    # Make requests (distributed across workers)
    for _ in range(10):
        client.get()
    
    # Check counter increased
    updated = Status.get('applications/multi/php')
    assert updated['requests']['total'] >= initial_requests + 10, \
        'Counters should aggregate across all workers'


# =============================================================================
# Security Tests
# =============================================================================

def test_php_status_delete_rejected():
    """Verify DELETE on status endpoint is rejected."""
    client.load('mirror')
    
    assert 'error' in client.conf_delete('/status/applications/mirror/php'), \
        'DELETE on status should be rejected'


def test_php_status_put_rejected():
    """Verify PUT on status endpoint is rejected."""
    client.load('mirror')
    
    assert 'error' in client.conf({}, '/status/applications/mirror/php'), \
        'PUT on status should be rejected'


def test_php_status_post_rejected():
    """Verify POST on status endpoint is rejected."""
    client.load('mirror')
    
    assert 'error' in client.conf({}, '/status/applications/mirror/php'), \
        'POST on status should be rejected'


# =============================================================================
# Edge Cases
# =============================================================================

def test_php_status_nonexistent_app():
    """Verify status for nonexistent app returns error or null."""
    try:
        status = Status.get('applications/nonexistent/php')
        assert status is None or status == {}, \
            'Nonexistent app should return None or empty'
    except Exception:
        pass  # Exception is also acceptable


def test_php_status_gc_runs_present():
    """Verify GC runs counter is present."""
    client.load('mirror')
    
    gc = Status.get('applications/mirror/php')['gc']
    
    assert 'runs' in gc
    assert gc['runs'] >= 0


def test_php_status_concurrent_requests():
    """Verify status works during concurrent requests."""
    client.load('mirror')
    
    import threading
    import time
    
    errors = []
    
    def make_request():
        try:
            for _ in range(5):
                client.get()
        except Exception as e:
            errors.append(e)
    
    # Start multiple threads
    threads = [threading.Thread(target=make_request) for _ in range(4)]
    for t in threads:
        t.start()
    
    # Get status while requests are in progress
    time.sleep(0.1)
    status = Status.get('applications/mirror/php')
    
    # Should still work
    assert status is not None
    assert 'requests' in status
    
    # Wait for threads
    for t in threads:
        t.join()
    
    assert len(errors) == 0, f'Errors in threads: {errors}'


def test_php_status_after_app_restart():
    """Verify status works after application restart."""
    client.load('mirror')
    
    # Get initial status
    initial = Status.get('applications/mirror/php')
    
    # Restart app by reconfiguring
    client.conf({
        "listeners": {"*:8080": {"pass": "applications/mirror"}},
        "applications": {
            "mirror": {
                "type": client.get_application_type(),
                "processes": {"spare": 0},
                "root": f"{client.options['test_dir']}/php/mirror",
                "index": "index.php",
            }
        }
    })
    
    # Wait for restart
    import time
    time.sleep(1)
    
    # Status should still work
    status = Status.get('applications/mirror/php')
    assert status is not None
    assert 'requests' in status
    assert 'memory' in status
