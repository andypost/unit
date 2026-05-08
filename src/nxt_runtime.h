
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Valentin V. Bartenev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_RUNTIME_H_INCLUDED_
#define _NXT_RUNTIME_H_INCLUDED_


typedef void (*nxt_runtime_cont_t)(nxt_task_t *task, nxt_uint_t status);


struct nxt_runtime_s {
    nxt_mp_t               *mem_pool;

    nxt_array_t            *inherited_sockets;  /* of nxt_listen_socket_t */
    nxt_array_t            *listen_sockets;     /* of nxt_listen_socket_t */

    nxt_array_t            *services;           /* of nxt_service_t */
    nxt_array_t            *languages;          /* of nxt_app_lang_module_t */
    void                   *data;

    nxt_runtime_cont_t     start;

    nxt_str_t              hostname;

    nxt_file_name_t        *pid_file;

#if (NXT_TLS)
    const nxt_tls_lib_t    *tls;
#endif

    nxt_array_t            *thread_pools;       /* of nxt_thread_pool_t */
    nxt_runtime_cont_t     continuation;

    nxt_process_t          *mprocess;
    size_t                 nprocesses;
    nxt_thread_mutex_t     processes_mutex;
    nxt_lvlhsh_t           processes;           /* of nxt_process_t */

    nxt_port_t             *port_by_type[NXT_PROCESS_MAX];
    nxt_lvlhsh_t           ports;               /* of nxt_port_t */

    nxt_list_t             *log_files;          /* of nxt_file_t */

    uint32_t               last_engine_id;

    nxt_process_type_t     type;

    nxt_timer_t            timer;

    uint8_t                daemon;
    uint8_t                batch;
    uint8_t                status;
    uint8_t                is_pid_isolated;
    /* See nxt_port_quit_mode_t in nxt_port.h. */
    uint8_t                quit_mode;

    const char             *engine;
    uint32_t               engine_connections;
    uint32_t               auxiliary_threads;
    nxt_credential_t       user_cred;
    nxt_capabilities_t     capabilities;
    const char             *group;
    const char             *pid;
    const char             *log;
    const char             *modules;
    const char             *state;
    const char             *ver;
    const char             *ver_tmp;
    const char             *conf;
    const char             *conf_tmp;
    const char             *tmp;
    const char             *control;

    mode_t                 control_mode;
    const char             *control_user;
    const char             *control_group;

    nxt_str_t              certs;
    nxt_str_t              scripts;

    nxt_queue_t            engines;            /* of nxt_event_engine_t */

    nxt_sockaddr_t         *controller_listen;
    nxt_listen_socket_t    *controller_socket;
};



typedef nxt_int_t (*nxt_module_init_t)(nxt_thread_t *thr, nxt_runtime_t *rt);


nxt_int_t nxt_runtime_create(nxt_task_t *task);
void nxt_runtime_quit(nxt_task_t *task, nxt_uint_t status);

void nxt_runtime_event_engine_free(nxt_runtime_t *rt);

nxt_int_t nxt_runtime_thread_pool_create(nxt_thread_t *thr, nxt_runtime_t *rt,
    nxt_uint_t max_threads, nxt_nsec_t timeout);


void nxt_runtime_process_add(nxt_task_t *task, nxt_process_t *process);
void nxt_runtime_process_remove(nxt_runtime_t *rt, nxt_process_t *process);

nxt_process_t *nxt_runtime_process_find(nxt_runtime_t *rt, nxt_pid_t pid);

nxt_process_t *nxt_runtime_process_first(nxt_runtime_t *rt,
    nxt_lvlhsh_each_t *lhe);

void nxt_runtime_process_release(nxt_runtime_t *rt, nxt_process_t *process);

#define nxt_runtime_process_next(rt, lhe)                                     \
    nxt_lvlhsh_each(&rt->processes, lhe)

nxt_port_t *nxt_runtime_process_port_create(nxt_task_t *task, nxt_runtime_t *rt,
    nxt_pid_t pid, nxt_port_id_t id, nxt_process_type_t type);

void nxt_runtime_port_remove(nxt_task_t *task, nxt_port_t *port);
void nxt_runtime_stop_app_processes(nxt_task_t *task, nxt_runtime_t *rt);

/*
 * Allocate a NXT_PORT_MSG_QUIT body byte carrying quit_param.  Returns
 * NULL when quit_param == NXT_PORT_QUIT_NORMAL (no allocation; libunit
 * defaults to NORMAL when the QUIT message arrives without a payload)
 * or when allocation fails (degrades to NORMAL under memory pressure).
 */
nxt_buf_t *nxt_runtime_quit_buf(nxt_task_t *task, uint8_t quit_param);

/*
 * P5: notify the runtime that a conn just decremented
 * engine->active_conns_cnt during a graceful drain.  When the count
 * hits zero before graceful_timeout, the runtime posts nxt_runtime_exit
 * immediately rather than waiting on the timer.  Cheap no-op when the
 * engine is not currently draining.
 */
void nxt_runtime_drain_conn_completed(nxt_task_t *task,
    nxt_event_engine_t *engine);

/*
 * P5: walk engine->idle_connections and close each.  Exposed for
 * router worker engines (nxt_router_worker_thread_quit) so the same
 * idle-then-active drain shape that nxt_runtime_quit() applies on
 * the main engine also runs per worker thread.
 */
void nxt_runtime_close_idle_connections(nxt_event_engine_t *engine);

/*
 * P5: walk engine->active_connections and mark each connection so
 * its next nxt_h1p_request_close() takes the shutdown branch instead
 * of going back to keep-alive.  Returns the count.  Used by
 * nxt_runtime_quit() and by nxt_router_worker_thread_quit() so the
 * router worker engines drain symmetrically with the main process.
 */
nxt_uint_t nxt_runtime_drain_active_connections(nxt_task_t *task,
    nxt_event_engine_t *engine);

/*
 * P5: graceful_timeout timer handler.  Force-closes every still-
 * active conn on the engine and runs engine->graceful_done.
 * Exposed so per-engine setup paths can attach it to
 * engine->graceful_timer.handler.
 */
void nxt_runtime_graceful_timeout_handler(nxt_task_t *task, void *obj,
    void *data);

/*
 * P5: hard-coded graceful_timeout (ms) used by every engine that
 * arms graceful_timer.  TODO: surface under "settings" via
 * nxt_conf_validation.c — deferred to a follow-up to keep this PR
 * scoped to the coordinator.
 */
#define NXT_RUNTIME_GRACEFUL_TIMEOUT_DEFAULT  30000

NXT_EXPORT nxt_port_t *nxt_runtime_port_find(nxt_runtime_t *rt, nxt_pid_t pid,
    nxt_port_id_t port_id);


/* STUB */
nxt_int_t nxt_runtime_controller_socket(nxt_task_t *task, nxt_runtime_t *rt);

nxt_str_t *nxt_current_directory(nxt_mp_t *mp);

nxt_listen_socket_t *nxt_runtime_listen_socket_add(nxt_runtime_t *rt,
    nxt_sockaddr_t *sa);
nxt_int_t nxt_runtime_listen_sockets_create(nxt_task_t *task,
    nxt_runtime_t *rt);
nxt_int_t nxt_runtime_listen_sockets_enable(nxt_task_t *task,
    nxt_runtime_t *rt);
nxt_file_t *nxt_runtime_log_file_add(nxt_runtime_t *rt, nxt_str_t *name);

/* STUB */
void nxt_cdecl nxt_log_time_handler(nxt_uint_t level, nxt_log_t *log,
    const char *fmt, ...);

void nxt_stream_connection_init(nxt_task_t *task, void *obj, void *data);

nxt_int_t nxt_http_register_variables(void);
#if (NXT_HAVE_NJS)
void nxt_http_register_js_proto(nxt_js_conf_t *jcf);
#endif


#define nxt_runtime_process_each(rt, process)                                 \
    do {                                                                      \
        nxt_lvlhsh_each_t  _lhe;                                              \
        nxt_process_t      *_nxt;                                             \
                                                                              \
        for (process = nxt_runtime_process_first(rt, &_lhe);                  \
             process != NULL;                                                 \
             process = _nxt) {                                                \
                                                                              \
            _nxt = nxt_runtime_process_next(rt, &_lhe);                       \

#define nxt_runtime_process_loop                                              \
        }                                                                     \
    } while(0)


extern nxt_module_init_t  nxt_init_modules[];
extern nxt_uint_t         nxt_init_modules_n;


#endif /* _NXT_RUNTIME_H_INCLIDED_ */
