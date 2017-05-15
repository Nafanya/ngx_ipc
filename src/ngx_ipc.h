#ifndef NGX_IPC_H
#define NGX_IPC_H

#include <ngx_core.h>
#include <ngx_ipc_shmem.h>

/**
  * Each IPC message is packed as following:
  * <SRC_SLOT(int32)> <TARGET_MODULE(int32)> <DATA_LEN(uint32)> <DATA>
  */

#define IPC_HEADER_LEN       (sizeof(ngx_int_t) + sizeof(ngx_int_t) + sizeof(size_t))
#define IPC_MAX_READ_BYTES   4096
#define IPC_DEFAULT_RBUF_LEN 512

typedef struct ngx_ipc_msg_queue_s       ngx_ipc_msg_queue_t;
typedef struct ngx_ipc_worker_slot_s     ngx_ipc_worker_slot_t;
typedef struct ngx_ipc_shm_data_s        ngx_ipc_shm_data_t;
typedef struct ngx_ipc_msg_link_s        ngx_ipc_msg_link_t;
typedef struct ngx_ipc_writebuf_s        ngx_ipc_writebuf_t;
typedef struct ngx_ipc_msg_waiting_s     ngx_ipc_msg_waiting_t;
typedef struct ngx_ipc_s                 ngx_ipc_t;
typedef struct ngx_ipc_readbuf_s         ngx_ipc_readbuf_t;
typedef struct ngx_ipc_process_s         ngx_ipc_process_t;

struct ngx_ipc_msg_queue_s {
    ngx_ipc_msg_waiting_t  *head;
    ngx_ipc_msg_waiting_t  *tail;
};

struct ngx_ipc_worker_slot_s {
    ngx_int_t        pid;
    ngx_int_t        slot;
};

struct ngx_ipc_shm_data_s {
    ngx_ipc_worker_slot_t   *worker_slots;
    void                    *ptr;
};

struct ngx_ipc_msg_link_s {
    ngx_ipc_msg_link_t      *next;
    ngx_str_t                buf;
    size_t                   sent;
};

struct ngx_ipc_writebuf_s {
    ngx_ipc_msg_link_t       *head;
    ngx_ipc_msg_link_t       *tail;
    size_t                    n;
};

struct ngx_ipc_readbuf_s {

    struct {
        ngx_int_t   slot;
        ngx_uint_t  size;
        ngx_int_t   module;
        u_char      buf[IPC_HEADER_LEN];
        ngx_int_t   bp;

        unsigned    complete:1;
    } header;

    u_char         *buf;
    ngx_uint_t      bp;
};

struct ngx_ipc_msg_waiting_s {
    ngx_int_t               sender_slot;
    ngx_pid_t               sender_pid;
    ngx_str_t               name;
    ngx_str_t               data;
    ngx_ipc_msg_waiting_t  *next;
};

struct ngx_ipc_process_s {
    ngx_ipc_t             *ipc;
    ngx_socket_t           pipe[2];
    ngx_connection_t      *c;
    ngx_ipc_writebuf_t     wbuf;
    ngx_ipc_readbuf_t      rbuf;

    unsigned               active:1;
};

struct ngx_ipc_s {
    const char            *name;

    ngx_ipc_process_t      process[NGX_MAX_PROCESSES];

    void                 (*handler)(ngx_int_t slot, ngx_int_t module, ngx_str_t *data);
};

ngx_int_t ngx_ipc_broadcast_msg(ngx_int_t module, ngx_str_t *data);
ngx_int_t ngx_ipc_send_msg(ngx_int_t target_worker, ngx_int_t module, ngx_str_t *data);

#endif //NGX_IPC_H
