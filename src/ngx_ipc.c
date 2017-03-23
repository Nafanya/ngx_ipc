#include <ngx_core.h>

#include "ngx_rtmp_stats.h"
#include "ngx_ipc.h"
#include "ngx_ipc_core.h"
#include "ngx_ipc_shmem.h"

#include <assert.h>

#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "JUJU | " fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "IPC | " fmt, ##args)

static shmem_t         *shm = NULL;
static shm_data_t      *shdata = NULL;

static ipc_t            ipc_data;
static ipc_t           *ipc = NULL;

static ngx_int_t        max_workers;
static ngx_log_t       *cycle_log;

//received but yet-unprocessed alerts are quered here
static struct {
    ipc_alert_waiting_t  *head;
    ipc_alert_waiting_t  *tail;
} received_alerts = {NULL, NULL};

static void ngx_ipc_alert_handler(ngx_int_t sender, ngx_str_t *name, ngx_str_t *data);
static ngx_int_t ngx_ipc_init_postconfig(ngx_conf_t *cf);
static ngx_int_t ngx_ipc_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_ipc_init_worker(ngx_cycle_t *cycle);
static void ngx_ipc_exit_worker(ngx_cycle_t *cycle);
static void ngx_ipc_exit_master(ngx_cycle_t *cycle);

static ngx_command_t  ngx_ipc_commands[] = {
    ngx_null_command
};

static ngx_http_module_t  ngx_ipc_ctx = {
    NULL,                          /* preconfiguration */
    ngx_ipc_init_postconfig,       /* postconfiguration */
    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */
    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */
    NULL,                          /* create location configuration */
    NULL,                          /* merge location configuration */
};

ngx_module_t  ngx_ipc_module = {
    NGX_MODULE_V1,
    &ngx_ipc_ctx,                  /* module context */
    ngx_ipc_commands,              /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    ngx_ipc_init_module,           /* init module */
    ngx_ipc_init_worker,           /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    ngx_ipc_exit_worker,           /* exit process */
    ngx_ipc_exit_master,           /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t initialize_shm(ngx_shm_zone_t *zone, void *data) {
    shm_data_t         *d;
    if (data) { //zone being passed after restart
        zone->data = data;
        d = zone->data;
        shm_reinit(shm);
    } else {
        shm_init(shm);

        if ((d = shm_calloc(shm, sizeof(*d), "root shared data")) == NULL) {
            return NGX_ERROR;
        }

        zone->data = d;
        shdata = d;
    }

    if (shdata->worker_slots) {
        shm_free(shm, shdata->worker_slots);
    }
    shdata->worker_slots = shm_calloc(shm, sizeof(worker_slot_t) * max_workers, "worker slots");

    return NGX_OK;
}

int ngx_ipc_send_alert(ngx_int_t target_worker, ngx_str_t *name, ngx_str_t *data) {
    int            i;

    for (i = 0; i < max_workers; i++) {
        if (shdata->worker_slots[i].pid == target_worker) {
            ipc_alert(ipc, shdata->worker_slots[i].slot, name, data);
            break;
        }
    }
    return 0;
}

int ngx_ipc_broadcast_alert(ngx_str_t *name, ngx_str_t *data) {
    int            i;

    for (i = 0; i < max_workers; i++) {
        ipc_alert(ipc, shdata->worker_slots[i].slot, name, data);
    }

    return 0;
}

int ngx_ipc_broadcast_except_one_alert(ngx_int_t excluded_pid, ngx_str_t *name, ngx_str_t *data) {
    int            i;

    for (i = 0; i < max_workers; i++) {
        if (shdata->worker_slots[i].pid != excluded_pid) {
            ipc_alert(ipc, shdata->worker_slots[i].slot, name, data);
            break;
        }
    }
    return 0;
}

static void ngx_ipc_alert_handler(ngx_int_t sender_slot, ngx_str_t *name, ngx_str_t *data) {
    ipc_alert_waiting_t       *alert;
    int                        i;
    ngx_pid_t                  sender_pid = NGX_INVALID_PID;
    static u_char              nbuf[sizeof("response") - 1 + NGX_INT_T_LEN];
    ngx_int_t                  conn_id;
    ngx_pool_t                *pool;
    ngx_chain_t               *cl, *l;
    ngx_uint_t                 len;
    u_char                    *p;

    alert = ngx_alloc(sizeof(*alert) + name->len + data->len, ngx_cycle->log);
    assert(alert);

    //find sender process id
    for (i = 0; i < max_workers; i++) {
        if (shdata->worker_slots[i].slot == sender_slot) {
            sender_pid = shdata->worker_slots[i].pid;
            break;
        }
    }

    DBG("ngx_ipc_alert_handler sender_slot=%d, sender_pid=%ui, mypid=%ui",
        sender_slot, sender_pid, (ngx_uint_t)ngx_getpid());

    alert->sender_slot = sender_slot;
    alert->sender_pid = sender_pid;

    alert->name.data = (u_char *)&alert[1];
    alert->name.len = name->len;
    ngx_memcpy(alert->name.data, name->data, name->len);

    alert->data.data = alert->name.data + alert->name.len;
    alert->data.len = data->len;
    ngx_memcpy(alert->data.data, data->data, data->len);

    alert->next = NULL;

    if(received_alerts.tail) {
        received_alerts.tail->next = alert;
    }
    received_alerts.tail = alert;
    if (!received_alerts.head) {
        received_alerts.head = alert;
    }

    if (name->data[0] == 'c') { //collect:conn_id
        conn_id = ngx_atoi(data->data, data->len);
        if (conn_id == NGX_ERROR) {
            ERR("got 'collect' request from fd=-1");
        }

        p = ngx_snprintf(nbuf, sizeof(nbuf), "response%d", conn_id);
        ngx_str_t name1 = ngx_string(nbuf);
        name1.len = p - name1.data;

        pool = ngx_create_pool(1024 * 1024, cycle_log);

        cl = ngx_rtmp_stat_create_stats(pool);
        if (cl == NULL) {
            ngx_destroy_pool(pool);
            return;
        }
        len = 0;

        for (l = cl; l; l = l->next) {
            len += (l->buf->last - l->buf->pos);
        }
        DBG("created response with len=%ui", len);
        ngx_str_t response;
        response.len = len;
        if ((response.data = ngx_pcalloc(pool, len)) == NULL) {
            ngx_destroy_pool(pool);
        }
        p = response.data;
        for (l = cl; l; l = l->next) {
            ngx_memcpy(p, l->buf->pos, l->buf->last - l->buf->pos);
            p += (l->buf->last - l->buf->pos);
        }

        ngx_ipc_send_alert(sender_pid, &name1, &response);
        ngx_destroy_pool(pool);
    } else if (name->data[0] == 'r') {
        conn_id = ngx_atoi(name->data + 8, name->len - 8);

        ngx_rtmp_stat_handle_stat(conn_id, data);
    }
}

static ngx_int_t ngx_ipc_init_postconfig(ngx_conf_t *cf) {
    ngx_str_t              name = ngx_string("ngx_ipc");

    shm = shm_create(&name, &ngx_ipc_module, cf, 1024*1024, initialize_shm, &ngx_ipc_module);

    return NGX_OK;
}

static ngx_int_t ngx_ipc_init_module(ngx_cycle_t *cycle) {
    ngx_core_conf_t      *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    max_workers = ccf->worker_processes;
    cycle_log = cycle->log;

    if (ipc == NULL) {
        ipc = &ipc_data;
        ipc_init(ipc);
        ipc_set_handler(ipc, ngx_ipc_alert_handler);
    }
    ipc_open(ipc, cycle, ccf->worker_processes, NULL);
    return NGX_OK;
}

static ngx_int_t ngx_ipc_init_worker(ngx_cycle_t *cycle) {
    int i, found = 0;
    shmtx_lock(shm);
    for (i = 0; i < max_workers; i++) {
        if (shdata->worker_slots[i].pid == 0) {
            shdata->worker_slots[i].pid = ngx_pid;
            shdata->worker_slots[i].slot = ngx_process_slot;
            found = 1;
            break;
        }
    }
    shmtx_unlock(shm);

    if (!found) {
        return NGX_ERROR;
    }
    ipc_register_worker(ipc, cycle);

    return NGX_OK;
}

static void ngx_ipc_exit_worker(ngx_cycle_t *cycle) {
    ipc_close(ipc, cycle);
}

static void ngx_ipc_exit_master(ngx_cycle_t *cycle) {
    ipc_close(ipc, cycle);
    shm_free(shm, shdata);
    shm_destroy(shm);
}
