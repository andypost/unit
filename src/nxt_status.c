
/*
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_conf.h>
#include <nxt_status.h>
#include <nxt_application.h>
#include <nxt_php_status.h>


/*
 * Convert PHP status structure to JSON configuration object.
 * Used for /status/applications/<name>/php endpoint.
 */
nxt_conf_value_t *
nxt_php_status_to_json(nxt_php_status_t *php_stats, nxt_mp_t *mp)
{
    nxt_conf_value_t  *php_obj, *opcache_obj, *jit_obj, *requests_obj,
                      *gc_obj, *memory_obj;

    static const nxt_str_t  opcache_str = nxt_string("opcache");
    static const nxt_str_t  jit_str = nxt_string("jit");
    static const nxt_str_t  requests_str = nxt_string("requests");
    static const nxt_str_t  gc_str = nxt_string("gc");
    static const nxt_str_t  memory_str = nxt_string("memory");

    static const nxt_str_t  enabled_str = nxt_string("enabled");
    static const nxt_str_t  hits_str = nxt_string("hits");
    static const nxt_str_t  misses_str = nxt_string("misses");
    static const nxt_str_t  cached_scripts_str = nxt_string("cached_scripts");
    static const nxt_str_t  memory_used_str = nxt_string("memory_used");
    static const nxt_str_t  memory_free_str = nxt_string("memory_free");
    static const nxt_str_t  interned_used_str = nxt_string("interned_strings_used");
    static const nxt_str_t  interned_free_str = nxt_string("interned_strings_free");

    static const nxt_str_t  buffer_size_str = nxt_string("buffer_size");
    static const nxt_str_t  jit_memory_used_str = nxt_string("memory_used");

    static const nxt_str_t  total_str = nxt_string("total");
    static const nxt_str_t  active_str = nxt_string("active");
    static const nxt_str_t  rejected_str = nxt_string("rejected");

    static const nxt_str_t  runs_str = nxt_string("runs");
    static const nxt_str_t  last_run_time_str = nxt_string("last_run_time");

    static const nxt_str_t  peak_str = nxt_string("peak");
    static const nxt_str_t  current_str = nxt_string("current");

    php_obj = nxt_conf_create_object(mp, 5);
    if (nxt_slow_path(php_obj == NULL)) {
        return NULL;
    }

    /* Opcache section */
    opcache_obj = nxt_conf_create_object(mp, 8);
    if (nxt_slow_path(opcache_obj == NULL)) {
        return NULL;
    }

    nxt_conf_set_member_integer(opcache_obj, &enabled_str,
                                php_stats->opcache_enabled, 0);
    nxt_conf_set_member_integer(opcache_obj, &hits_str,
                                php_stats->opcache_hits, 1);
    nxt_conf_set_member_integer(opcache_obj, &misses_str,
                                php_stats->opcache_misses, 2);
    nxt_conf_set_member_integer(opcache_obj, &cached_scripts_str,
                                php_stats->opcache_cached_scripts, 3);
    nxt_conf_set_member_integer(opcache_obj, &memory_used_str,
                                php_stats->opcache_memory_used, 4);
    nxt_conf_set_member_integer(opcache_obj, &memory_free_str,
                                php_stats->opcache_memory_free, 5);
    nxt_conf_set_member_integer(opcache_obj, &interned_used_str,
                                php_stats->opcache_interned_strings_used, 6);
    nxt_conf_set_member_integer(opcache_obj, &interned_free_str,
                                php_stats->opcache_interned_strings_free, 7);

    nxt_conf_set_member(php_obj, &opcache_str, opcache_obj, 0);

    /* JIT section */
    jit_obj = nxt_conf_create_object(mp, 3);
    if (nxt_slow_path(jit_obj == NULL)) {
        return NULL;
    }

    nxt_conf_set_member_integer(jit_obj, &enabled_str,
                                php_stats->jit_enabled, 0);
    nxt_conf_set_member_integer(jit_obj, &buffer_size_str,
                                php_stats->jit_buffer_size, 1);
    nxt_conf_set_member_integer(jit_obj, &jit_memory_used_str,
                                php_stats->jit_memory_used, 2);

    nxt_conf_set_member(php_obj, &jit_str, jit_obj, 1);

    /* Requests section */
    requests_obj = nxt_conf_create_object(mp, 3);
    if (nxt_slow_path(requests_obj == NULL)) {
        return NULL;
    }

    nxt_conf_set_member_integer(requests_obj, &total_str,
                                php_stats->requests_total, 0);
    nxt_conf_set_member_integer(requests_obj, &active_str,
                                php_stats->requests_active, 1);
    nxt_conf_set_member_integer(requests_obj, &rejected_str,
                                php_stats->requests_rejected, 2);

    nxt_conf_set_member(php_obj, &requests_str, requests_obj, 2);

    /* GC section */
    gc_obj = nxt_conf_create_object(mp, 2);
    if (nxt_slow_path(gc_obj == NULL)) {
        return NULL;
    }

    nxt_conf_set_member_integer(gc_obj, &runs_str,
                                php_stats->gc_runs, 0);
    nxt_conf_set_member_integer(gc_obj, &last_run_time_str,
                                php_stats->gc_last_run_time, 1);

    nxt_conf_set_member(php_obj, &gc_str, gc_obj, 3);

    /* Memory section */
    memory_obj = nxt_conf_create_object(mp, 2);
    if (nxt_slow_path(memory_obj == NULL)) {
        return NULL;
    }

    nxt_conf_set_member_integer(memory_obj, &peak_str,
                                php_stats->memory_peak, 0);
    nxt_conf_set_member_integer(memory_obj, &current_str,
                                php_stats->memory_current, 1);

    nxt_conf_set_member(php_obj, &memory_str, memory_obj, 4);

    return php_obj;
}


nxt_conf_value_t *
nxt_status_get(nxt_status_report_t *report, nxt_mp_t *mp)
{
    size_t                 i, nr_langs;
    uint16_t               lang_cnts[NXT_APP_UNKNOWN] = { 1 };
    uint32_t               idx = 0;
    nxt_str_t              name;
    nxt_int_t              ret;
    nxt_array_t            *langs;
    nxt_thread_t           *thr;
    nxt_app_type_t         type, prev_type;
    nxt_status_app_t       *app;
    nxt_conf_value_t       *status, *obj, *mods, *apps, *app_obj, *mod_obj;
    nxt_app_lang_module_t  *modules;

    static const nxt_str_t  modules_str = nxt_string("modules");
    static const nxt_str_t  version_str = nxt_string("version");
    static const nxt_str_t  lib_str = nxt_string("lib");
    static const nxt_str_t  conns_str = nxt_string("connections");
    static const nxt_str_t  acc_str = nxt_string("accepted");
    static const nxt_str_t  active_str = nxt_string("active");
    static const nxt_str_t  idle_str = nxt_string("idle");
    static const nxt_str_t  closed_str = nxt_string("closed");
    static const nxt_str_t  reqs_str = nxt_string("requests");
    static const nxt_str_t  total_str = nxt_string("total");
    static const nxt_str_t  apps_str = nxt_string("applications");
    static const nxt_str_t  procs_str = nxt_string("processes");
    static const nxt_str_t  run_str = nxt_string("running");
    static const nxt_str_t  start_str = nxt_string("starting");

    status = nxt_conf_create_object(mp, 4);
    if (nxt_slow_path(status == NULL)) {
        return NULL;
    }

    thr = nxt_thread();
    langs = thr->runtime->languages;

    modules = langs->elts;
    /*
     * We need to count the number of unique languages to correctly
     * allocate the below mods object.
     *
     * We also need to count how many of each language.
     *
     * Start by skipping past NXT_APP_EXTERNAL which is always the
     * first entry.
     */
    for (i = 1, nr_langs = 0, prev_type = NXT_APP_UNKNOWN; i < langs->nelts;
         i++)
    {
        type = modules[i].type;

        lang_cnts[type]++;

        if (type == prev_type) {
            continue;
        }

        nr_langs++;
        prev_type = type;
    }

    mods = nxt_conf_create_object(mp, nr_langs);
    if (nxt_slow_path(mods == NULL)) {
        return NULL;
    }

    nxt_conf_set_member(status, &modules_str, mods, idx++);

    i = 1;
    obj = mod_obj = NULL;
    prev_type = NXT_APP_UNKNOWN;
    for (size_t l = 0, a = 0; i < langs->nelts; i++) {
        nxt_str_t  item, mod_name;

        type = modules[i].type;
        if (type != prev_type) {
            a = 0;

            if (lang_cnts[type] == 1) {
                mod_obj = nxt_conf_create_object(mp, 2);
                obj = mod_obj;
            } else {
                mod_obj = nxt_conf_create_array(mp, lang_cnts[type]);
            }

            if (nxt_slow_path(mod_obj == NULL)) {
                return NULL;
            }

            mod_name.start = (u_char *)modules[i].name;
            mod_name.length = strlen(modules[i].name);
            nxt_conf_set_member(mods, &mod_name, mod_obj, l++);
        }

        if (lang_cnts[type] > 1) {
            obj = nxt_conf_create_object(mp, 2);
            if (nxt_slow_path(obj == NULL)) {
                return NULL;
            }

            nxt_conf_set_element(mod_obj, a++, obj);
        }

        item.start = modules[i].version;
        item.length = nxt_strlen(modules[i].version);
        nxt_conf_set_member_string(obj, &version_str, &item, 0);

        item.start = (u_char *)modules[i].file;
        item.length = strlen(modules[i].file);
        nxt_conf_set_member_string(obj, &lib_str, &item, 1);

        prev_type = type;
    }

    obj = nxt_conf_create_object(mp, 4);
    if (nxt_slow_path(obj == NULL)) {
        return NULL;
    }

    nxt_conf_set_member(status, &conns_str, obj, idx++);

    nxt_conf_set_member_integer(obj, &acc_str, report->accepted_conns, 0);
    nxt_conf_set_member_integer(obj, &active_str, report->accepted_conns
                                                  - report->closed_conns
                                                  - report->idle_conns, 1);
    nxt_conf_set_member_integer(obj, &idle_str, report->idle_conns, 2);
    nxt_conf_set_member_integer(obj, &closed_str, report->closed_conns, 3);

    obj = nxt_conf_create_object(mp, 1);
    if (nxt_slow_path(obj == NULL)) {
        return NULL;
    }

    nxt_conf_set_member(status, &reqs_str, obj, idx++);

    nxt_conf_set_member_integer(obj, &total_str, report->requests, 0);

    apps = nxt_conf_create_object(mp, report->apps_count);
    if (nxt_slow_path(apps == NULL)) {
        return NULL;
    }

    nxt_conf_set_member(status, &apps_str, apps, idx++);

    for (i = 0; i < report->apps_count; i++) {
        app = &report->apps[i];

        app_obj = nxt_conf_create_object(mp, 3);
        if (nxt_slow_path(app_obj == NULL)) {
            return NULL;
        }

        name.length = app->name.length;
        name.start = nxt_pointer_to(report, (uintptr_t) app->name.start);

        ret = nxt_conf_set_member_dup(apps, mp, &name, app_obj, i);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NULL;
        }

        obj = nxt_conf_create_object(mp, 3);
        if (nxt_slow_path(obj == NULL)) {
            return NULL;
        }

        nxt_conf_set_member(app_obj, &procs_str, obj, 0);

        nxt_conf_set_member_integer(obj, &run_str, app->processes, 0);
        nxt_conf_set_member_integer(obj, &start_str, app->pending_processes, 1);
        nxt_conf_set_member_integer(obj, &idle_str, app->idle_processes, 2);

        obj = nxt_conf_create_object(mp, 1);
        if (nxt_slow_path(obj == NULL)) {
            return NULL;
        }

        nxt_conf_set_member(app_obj, &reqs_str, obj, 1);

        nxt_conf_set_member_integer(obj, &active_str, app->active_requests, 0);

        /*
         * Runtime statistics (opt-in, security-sensitive)
         * Exposes: memory, GC, language-specific stats (opcache, etc.)
         * See: PHP_STATUS_TODO.md for security considerations
         */
        {
            nxt_conf_value_t  *runtime_obj;

            runtime_obj = nxt_conf_create_object(mp, 3);
            if (nxt_slow_path(runtime_obj == NULL)) {
                return NULL;
            }

            /* Language type and version */
            static const nxt_str_t  type_str = nxt_string("type");
            static const nxt_str_t  version_str = nxt_string("version");
            static const nxt_str_t  php_str = nxt_string("php");
            static const nxt_str_t  php_version = nxt_string("8.5");

            nxt_conf_set_member_string(runtime_obj, &type_str, &php_str, 0);
            nxt_conf_set_member_string(runtime_obj, &version_str, &php_version, 1);

            /* Collect runtime stats from PHP module */
            {
                nxt_conf_value_t  *stats_obj;
                nxt_php_status_t  php_stats;

                nxt_php_collect_status(&php_stats);

                stats_obj = nxt_php_status_to_json(&php_stats, mp);
                if (stats_obj != NULL) {
                    static const nxt_str_t  stats_str = nxt_string("stats");
                    nxt_conf_set_member(runtime_obj, &stats_str, stats_obj, 2);
                }
            }

            /* Add runtime section to app object */
            static const nxt_str_t  runtime_str = nxt_string("runtime");
            nxt_conf_set_member(app_obj, &runtime_str, runtime_obj, 2);
        }
    }

    return status;
}
