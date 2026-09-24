# USDT probes

## Building

```
apt-get install -y systemtap-sdt-dev   # <sys/sdt.h>
./configure --usdt ...
make -j2
```

Without `--usdt`, `NXT_USDT()` (`src/nxt_usdt.h`) compiles to nothing; with
it, each probe is a `nop` until a tracer attaches.

## Probes

| Probe | Call site | Arguments |
|---|---|---|
| `freeunit:port-send` | `nxt_port_socket_write2()` | `stream, type` |
| `freeunit:port-recv` | `nxt_port_read_handler()`, once per message read | `port->pid` |
| `freeunit:mmap-chunk-alloc` | `nxt_port_incoming_port_mmap()` | `process->pid, PORT_MMAP_SIZE` |
| `freeunit:mmap-chunk-get` | `nxt_router_prepare_msg()` | `req_size + content_length` |
| `freeunit:queue-enqueue` | `nxt_app_queue_send()` | `slot index, tracking id` |
| `freeunit:queue-dequeue` | `nxt_app_queue_recv()`, in the application process | `slot index` |
| `freeunit:process-spawn` | `nxt_process_create()`, parent only | `child pid` (the global one, also with pid isolation) |
| `freeunit:request-start` | `nxt_http_request_create()` | `(uintptr_t) r` |
| `freeunit:request-done` | `nxt_http_request_done()` | `(uintptr_t) r, status` |

## Example

```
bpftrace -l 'usdt:/usr/sbin/unitd:*'
bpftrace tools/usdt/requests-by-status.bt -p $(pgrep -f 'unit: router')
```

`tools/usdt/` also has `port-rtt.bt` and `queue-residency.bt`. The scripts
have not been run against a live process yet.
