#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_ipc.h"
#include "ngx_ipc_core.h"
#include "ngx_ipc_shmem.h"


#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "ipc: " fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "ipc: " fmt, ##args)

static shmem_t         *shm = NULL;
static shm_data_t      *shdata = NULL;

static ipc_t            ipc_data;
static ipc_t           *ipc = NULL;

static ngx_int_t        max_workers;
static ngx_log_t       *cycle_log;

typedef struct ipc_msg_queue_s ipc_msg_queue_t;

struct ipc_msg_queue_s {
    ipc_msg_waiting_t  *head;
    ipc_msg_waiting_t  *tail;
};

static ipc_msg_queue_t *received_messages;

static void ngx_ipc_msg_handler(ngx_int_t sender, ngx_int_t module, ngx_str_t *data);
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
    if (data) {
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

int ngx_ipc_send_msg(ngx_int_t target_worker, ngx_int_t module, ngx_str_t *data) {
    int i;

    for (i = 0; i < max_workers; i++) {
        if (shdata->worker_slots[i].slot == target_worker) {
            ipc_send_msg(ipc, shdata->worker_slots[i].slot, module, data);
            break;
        }
    }
    return 0;
}

int ngx_ipc_broadcast_msg(ngx_int_t module, ngx_str_t *data) {
    int i;

    for (i = 0; i < max_workers; i++) {
        ipc_send_msg(ipc, shdata->worker_slots[i].slot, module, data);
//        ngx_msleep(50);
    }

    return 0;
}

static void ngx_ipc_msg_handler(ngx_int_t sender_slot, ngx_int_t module, ngx_str_t *data) {
    int i;

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                  "ipc: message %d<-%d module=%d len=%ui",
                  ngx_process_slot, sender_slot, module, data->len);

    if (data->len > 0 && data->data[0] == 'h') {
        ngx_str_t r;
        r.data = ngx_alloc(1024*1024, ngx_cycle->log);
        if (r.data == NULL) {
            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                          "ipc: send PONG failed dure to nomem");
        }
        r.len = 1024*1024;
        for (i = 0; i < 1024*1024; i++) {
            r.data[i] = 'a';
        }

//        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
//                      "ipc: send PONG from slot=%d module=%d data.len=%d", sender_slot, module, r.len);
//        ngx_ipc_broadcast_msg(ngx_ipc_module.index, &r);
        ngx_ipc_send_msg(sender_slot, 1, &r);

        ngx_free(r.data);
    }
}

static ngx_int_t ngx_ipc_init_postconfig(ngx_conf_t *cf) {
    ngx_str_t              name = ngx_string("ngx_ipc");

    shm = shm_create(&name, &ngx_ipc_module, cf, 1024*1024, initialize_shm, &ngx_ipc_module);

    return NGX_OK;
}

static ngx_int_t ngx_ipc_init_module(ngx_cycle_t *cycle) {
    ngx_core_conf_t      *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    ngx_uint_t i;

    max_workers = ccf->worker_processes;
    cycle_log = cycle->log;

    if (ipc == NULL) {
        ipc = &ipc_data;
        ipc_init(ipc);
        ipc_set_handler(ipc, ngx_ipc_msg_handler);
    }
    ipc_open(ipc, cycle, ccf->worker_processes, NULL);

    received_messages = ngx_pcalloc(ngx_cycle->pool, sizeof(ipc_msg_queue_t) * ngx_cycle->modules_n);
    for (i = 0; i < ngx_cycle->modules_n; i++) {
        received_messages[i].head = NULL;
        received_messages[i].tail = NULL;
    }

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
