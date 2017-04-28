#ifndef NGX_IPC_CORE_H
#define NGX_IPC_CORE_H


#include <ngx_core.h>

/**
  * Each IPC message is packed as following:
  * <SRC_SLOT(int32)> <DATA_LEN(uint32)> <TARGET_MODULE(int32)> <DATA>
  */

#define IPC_HEADER_LEN    (sizeof(ngx_int_t) + sizeof(size_t) + sizeof(ngx_int_t))
#define IPC_MAX_READ_BYTES 4096
#define IPC_DEFAULT_RBUF_LEN 512

typedef struct ipc_msg_link_s        ipc_msg_link_t;
typedef struct ipc_writebuf_s        ipc_writebuf_t;
typedef struct ipc_msg_waiting_s     ipc_msg_waiting_t;
typedef struct ipc_s                 ipc_t;
typedef struct ipc_readbuf_s         ipc_readbuf_t;
typedef struct ipc_process_s         ipc_process_t;

struct ipc_msg_link_s {
    ipc_msg_link_t         *next;
    ngx_str_t               buf;
    size_t                  sent;
};

struct ipc_writebuf_s {
    ipc_msg_link_t       *head;
    ipc_msg_link_t       *tail;
    size_t                  n;
};

struct ipc_readbuf_s {

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

    unsigned    complete:1; //TODO: maybe remove
};

struct ipc_msg_waiting_s {
    ngx_int_t             sender_slot;
    ngx_pid_t             sender_pid;
    ngx_str_t             name;
    ngx_str_t             data;
    ipc_msg_waiting_t  *next;
};

struct ipc_process_s {
    ipc_t                 *ipc;
    ngx_socket_t           pipe[2];
    ngx_connection_t      *c;
    ipc_writebuf_t         wbuf;
    ipc_readbuf_t          rbuf;

    unsigned               active:1;
};

struct ipc_s {
    const char            *name;

    ipc_process_t         process[NGX_MAX_PROCESSES];

    void                  (*handler)(ngx_int_t slot, ngx_int_t module, size_t size, u_char *data);
};

ngx_int_t ipc_init(ipc_t *ipc);
ngx_int_t ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers, void (*slot_callback)(int slot, int worker));
ngx_int_t ipc_set_handler(ipc_t *ipc, void (*msg_handler)(ngx_int_t, ngx_int_t, size_t, u_char *));
ngx_int_t ipc_register_worker(ipc_t *ipc, ngx_cycle_t *cycle);
ngx_int_t ipc_close(ipc_t *ipc, ngx_cycle_t *cycle);

ngx_int_t ipc_send_msg(ipc_t *ipc, ngx_int_t slot, ngx_int_t module_index, ngx_str_t *data);

#endif //NGX_IPC_CORE_H
