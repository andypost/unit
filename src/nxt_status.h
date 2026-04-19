
/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_STATUS_H_INCLUDED_
#define _NXT_STATUS_H_INCLUDED_


/*
 * Forward declaration for language-specific status data.
 * Each language module (PHP, Python, etc.) can define its own structure.
 */
typedef struct nxt_php_status_s  nxt_php_status_t;


typedef struct {
    nxt_str_t         name;
    uint32_t          active_requests;
    uint32_t          pending_processes;
    uint32_t          processes;
    uint32_t          idle_processes;

    /*
     * Language-specific statistics.
     * For PHP applications, points to nxt_php_status_t structure.
     * Allocated and freed by the respective language module.
     */
    void              *lang_stats;
    uint32_t          lang_stats_size;
} nxt_status_app_t;


typedef struct {
    uint64_t          accepted_conns;
    uint64_t          idle_conns;
    uint64_t          closed_conns;
    uint64_t          requests;

    size_t            apps_count;
    nxt_status_app_t  apps[];
} nxt_status_report_t;


nxt_conf_value_t *nxt_status_get(nxt_status_report_t *report, nxt_mp_t *mp);
nxt_conf_value_t *nxt_php_status_to_json(nxt_php_status_t *php_stats, nxt_mp_t *mp);


#endif /* _NXT_STATUS_H_INCLUDED_ */
