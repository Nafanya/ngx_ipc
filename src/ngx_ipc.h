#ifndef NGX_IPC_H
#define NGX_IPC_H

#include <ngx_core.h>

extern ngx_module_t ngx_ipc_module;

typedef struct worker_slot_s     worker_slot_t;
typedef struct shm_data_s        shm_data_t;

struct worker_slot_s {
    ngx_int_t        pid;
    ngx_int_t        slot;
};

struct shm_data_s {
    worker_slot_t   *worker_slots;
    void            *ptr;
};

int ngx_ipc_broadcast_msg(ngx_int_t module, ngx_str_t *data);

#endif //NGX_IPC_H
