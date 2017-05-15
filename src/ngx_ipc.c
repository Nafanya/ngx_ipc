#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_ipc.h"
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

static ngx_int_t ngx_ipc_init_postconfig(ngx_conf_t *cf);
static ngx_int_t ngx_ipc_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_ipc_init_worker(ngx_cycle_t *cycle);
static void      ngx_ipc_exit_worker(ngx_cycle_t *cycle);
static void      ngx_ipc_exit_master(ngx_cycle_t *cycle);

static void      ngx_ipc_msg_handler(ngx_int_t sender, ngx_int_t module, ngx_str_t *data);
static ngx_int_t ngx_ipc_reset_readbuf(ipc_readbuf_t *b);
static void      ngx_ipc_try_close_fd(ngx_socket_t *fd);

static ngx_int_t ngx_ipc_init(ipc_t *ipc);
static ngx_int_t ngx_ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers,
                              void (*slot_callback)(int slot, int worker));
static ngx_int_t ngx_ipc_close(ipc_t *ipc, ngx_cycle_t *cycle);

static ngx_int_t ngx_ipc_write_buffered_msg(ngx_socket_t fd, ipc_msg_link_t *msg);
static ngx_int_t ngx_ipc_free_buffered_msg(ipc_msg_link_t *msg_link);
static void      ngx_ipc_write_handler(ngx_event_t *ev);
static ngx_int_t ngx_ipc_read(ipc_process_t *ipc_proc, ipc_readbuf_t *rbuf, ngx_log_t *log);
static void      ngx_ipc_read_handler(ngx_event_t *ev);

//TODO: int -> ngx_int_t
static ngx_int_t ipc_send_msg(ipc_t *ipc, ngx_int_t slot, ngx_int_t module_index, ngx_str_t *data);
static    ngx_int_t initialize_shm(ngx_shm_zone_t *zone, void *data);


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


static ngx_int_t ngx_ipc_init(ipc_t *ipc) {
    int             i;
    ipc_process_t  *proc;

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        proc = &ipc->process[i];
        proc->ipc = ipc;
        proc->pipe[0] = NGX_INVALID_FILE;
        proc->pipe[1] = NGX_INVALID_FILE;
        proc->c = NULL;
        proc->active = 0;
        proc->wbuf.head = NULL;
        proc->wbuf.tail = NULL;
        proc->wbuf.n = 0;
        ngx_ipc_reset_readbuf(&proc->rbuf);
    }
    return NGX_OK;
}

static ngx_int_t ngx_ipc_reset_readbuf(ipc_readbuf_t *b) {
//    ngx_memzero(&b->header, sizeof(b->header));
    b->header.bp = 0;
    b->header.complete = 0;
    b->header.module = 0;
    b->header.size = 0;
    b->header.slot = 0;

    b->bp = 0;

    if (b->buf) {
        ngx_free(b->buf);
        b->buf = NULL;
    }

    return NGX_OK;
}

static void ngx_ipc_try_close_fd(ngx_socket_t *fd) {
    if (*fd != NGX_INVALID_FILE) {
        ngx_close_socket(*fd);
        *fd = NGX_INVALID_FILE;
    }
}

static ngx_int_t ngx_ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers,
                   void (*slot_callback)(int slot, int worker)) {
    //initialize pipes for workers in advance.
    int              i, j, s = 0;
    ngx_int_t        last_expected_process = ngx_last_process;
    ipc_process_t   *proc;
    ngx_socket_t    *socks;

    /* here's the deal: we have no control over fork()ing, nginx's internal
    * socketpairs are unusable for our purposes (as of nginx 0.8 -- check the
    * code to see why), and the module initialization callbacks occur before
    * any workers are spawned. Rather than futzing around with existing
    * socketpairs, we make our own pipes array.
    * Trouble is, ngx_spawn_process() creates them one-by-one, and we need to
    * do it all at once. So we must guess all the workers' ngx_process_slots in
    * advance. Meaning the spawning logic must be copied to the T.
    * ... with some allowances for already-opened sockets...
    */
    for (i = 0; i < workers; i++) {
        //copypaste from os/unix/ngx_process.c (ngx_spawn_process)
        while (s < last_expected_process && ngx_processes[s].pid != -1) {
            //find empty existing slot
            s++;
        }

        if (slot_callback) {
            slot_callback(s, i);
        }

        proc = &ipc->process[s];

        socks = proc->pipe;

        if (proc->active) {
            // reinitialize already active pipes. This is done to prevent IPC alerts
            // from a previous restart that were never read from being received by
            // a newly restarted worker
            ngx_ipc_try_close_fd(&socks[0]);
            ngx_ipc_try_close_fd(&socks[1]);
            proc->active = 0;
        }

        if (pipe(socks) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "pipe() failed while initializing ipc");
            return NGX_ERROR;
        }

        for (j = 0; j <= 1; j++) {
            if (ngx_nonblocking(socks[j]) == -1) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              ngx_nonblocking_n " failed on pipe socket %i while initializing ipc", j);
                ngx_ipc_try_close_fd(&socks[0]);
                ngx_ipc_try_close_fd(&socks[1]);
                return NGX_ERROR;
            }
        }
        proc->active = 1;

        s++;
    }

    return NGX_OK;
}

static ngx_int_t ngx_ipc_close(ipc_t *ipc, ngx_cycle_t *cycle) {
    int i;
    ipc_process_t   *proc;
    ipc_msg_link_t  *cur, *cur_next;

    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        proc = &ipc->process[i];
        if (!proc->active) continue;

        if (proc->c) {
            ngx_close_connection(proc->c);
            proc->c = NULL;
        }

        for (cur = proc->wbuf.head; proc->wbuf.n > 0; proc->wbuf.n--) {
            cur_next = cur->next;
            ngx_ipc_free_buffered_msg(cur);
            cur = cur_next;
        }

        ngx_ipc_try_close_fd(&proc->pipe[0]);
        ngx_ipc_try_close_fd(&proc->pipe[1]);
        ipc->process[i].active = 0;
    }
    return NGX_OK;
}

static ngx_int_t ipc_register_worker(ipc_t *ipc, ngx_cycle_t *cycle) {
    int                    i;
    ngx_connection_t      *c;
    ipc_process_t         *proc;

    for (i = 0; i< NGX_MAX_PROCESSES; i++) {

        proc = &ipc->process[i];

        if (!proc->active) continue;

        if (i == ngx_process_slot) {
            c = ngx_get_connection(proc->pipe[0], cycle->log);
            c->data = ipc;

            c->read->handler = ngx_ipc_read_handler;
            c->read->log = cycle->log;
            c->write->handler = NULL;

            ngx_add_event(c->read, NGX_READ_EVENT, 0);
            proc->c=c;
        } else {
            c = ngx_get_connection(proc->pipe[1], cycle->log);

            c->data = proc;

            c->read->handler = NULL;
            c->write->log = cycle->log;
            c->write->handler = ngx_ipc_write_handler;

            proc->c=c;
        }
    }
    return NGX_OK;
}

static ngx_int_t ngx_ipc_write_buffered_msg(ngx_socket_t fd, ipc_msg_link_t *msg) {
    ssize_t      n;
    ngx_int_t    err;
    ssize_t      unsent;
    u_char      *data;

    while (1) {
        unsent = msg->buf.len - msg->sent;
        data = msg->buf.data + msg->sent;

        if (unsent == 0) {
            break;
        }

        n = write(fd, data, unsent);

//        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
//                      "\t\tipc: WRITE unsent=%ui sent=%d", unsent, (int)n);

        if (n == -1) {
            err = ngx_errno;
            if (err == NGX_EAGAIN) {
                return NGX_AGAIN;
            }

            ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err, "write() failed");
            return NGX_ERROR;
        } else if (n < unsent) {
            msg->sent += n;
        } else {
            break;
        }
    }

    return NGX_OK;
}

static ngx_int_t ngx_ipc_free_buffered_msg(ipc_msg_link_t *msg_link) {
    ngx_free(msg_link->buf.data);
    ngx_free(msg_link);
    return NGX_OK;
}

static void ngx_ipc_write_handler(ngx_event_t *ev) {
    ngx_connection_t *c = ev->data;
    ngx_socket_t      fd = c->fd;

    ipc_process_t    *proc = (ipc_process_t *) c->data;
    ipc_msg_link_t   *cur;

    ngx_int_t         rc;
    uint8_t           aborted = 0;

    while (proc->wbuf.n > 0) {
        cur = proc->wbuf.head;
        rc = ngx_ipc_write_buffered_msg(fd, cur);
        if (rc == NGX_AGAIN) {
//            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "\tWRITE wbuf NGX_AGAIN");
            aborted = 1;
            break;
        } else if (rc == NGX_OK) {
//            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "\tWRITE wbuf OK");

            proc->wbuf.n--;
            if (proc->wbuf.n == 0) {
                proc->wbuf.head = proc->wbuf.tail = NULL;
            } else {
                proc->wbuf.head = cur->next;
            }
            ngx_ipc_free_buffered_msg(cur);
        } else {
            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "\tWRITE aborted ret=%d", rc);
            aborted = 1;
            break;
        }
    }

    if (aborted) {
        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "\tWRITE aborted, handle_write_event {");
        ngx_handle_write_event(c->write, 0);
        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "\tWRITE aborted, handle_write_event }");
    }
}

static ngx_int_t ngx_ipc_read(ipc_process_t *ipc_proc, ipc_readbuf_t *rbuf, ngx_log_t *log) {
    ngx_int_t       n, i;
    ngx_err_t       err;
    ngx_socket_t    s = ipc_proc->c->fd;
    u_char         *c;

    while (!rbuf->header.complete) {
//        ngx_log_error(NGX_LOG_INFO, log, 0, "\t\tipc: header is not complete: h.bp=%d", rbuf->header.bp);
        n = read(s, rbuf->header.buf + rbuf->header.bp, IPC_HEADER_LEN - rbuf->header.bp);
        if (n == -1) {
            err = ngx_errno;
            if (err == NGX_EAGAIN) {
                return NGX_AGAIN;
            }
            ngx_log_error(NGX_LOG_ERR, log, err, "ipc: read() failed");
            return NGX_ERROR;
        } else if (n == 0) {
            ngx_log_error(NGX_LOG_ERR, log, 0, "ipc: read() returned 0");
            return NGX_ERROR;
        } else {
            rbuf->header.bp += n;
            if (rbuf->header.bp == IPC_HEADER_LEN) {
                c = rbuf->header.buf;

                //TODO: rewrite using macros or function
                for (i = 0, rbuf->header.slot = 0; i < (int)sizeof(rbuf->header.slot); i++) {
                    int shift = 8 * (sizeof(rbuf->header.slot) - i - 1);
                    rbuf->header.slot += (*c << shift);
                    c++;
                }
                for (i = 0, rbuf->header.module = 0; i < (int)sizeof(rbuf->header.module); i++) {
                    int shift = 8 * (sizeof(rbuf->header.module) - i - 1);
                    rbuf->header.module += (*c << shift);
                    c++;
                }
                for (i = 0, rbuf->header.size = 0; i < (int)sizeof(rbuf->header.size); i++) {
                    int shift = 8 * (sizeof(rbuf->header.size) - i - 1);
                    rbuf->header.size += (*c << shift);
                    c++;
                }
                rbuf->header.complete = 1;

//                ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
//                              "\t\tipc: ipc_read header parsed slot=%d, module=%d size=%ui",
//                              rbuf->header.slot, rbuf->header.module, rbuf->header.size);

                break;
            }
        }
    }

    if (rbuf->buf == NULL) {
        rbuf->buf = ngx_alloc(rbuf->header.size, ngx_cycle->log);
        if (rbuf->buf == NULL) {
            ngx_log_error(NGX_LOG_ERR, log, 0, "ipc: nomem %ui", rbuf->header.size);
            return NGX_ERROR;
        }
        rbuf->bp = 0;
    }

    while (rbuf->bp < rbuf->header.size) {
//        ngx_log_error(NGX_LOG_INFO, log, 0, "\t\tipc: READ bp=%ui rem=%ui NGX_AGAIN",
//                      rbuf->bp, rbuf->header.size - rbuf->bp);
        n = read(s, &rbuf->buf[rbuf->bp], rbuf->header.size - rbuf->bp);
        if (n == -1) {
            err = ngx_errno;
            if (err == NGX_EAGAIN) {
//                ngx_log_error(NGX_LOG_INFO, log, 0, "\t\tipc: READ body NGX_AGAIN");
                return NGX_AGAIN;
            }
            ngx_log_error(NGX_LOG_ERR, log, err, "ipc: read() failed");
            return NGX_ERROR;
        } else if (n == 0) {
            ngx_log_error(NGX_LOG_ERR, log, 0, "ipc: read() returned 0");
            return NGX_ERROR;
        } else {
//            ngx_log_error(NGX_LOG_INFO, log, 0, "\t\tipc: READ read=%d bp=%ui bpnew=%ui", n, rbuf->bp, rbuf->bp + n);
            rbuf->bp += n;
            if (rbuf->bp >= rbuf->header.size) {
//                for (n = 0; n < (int)rbuf->bp; n++) {
//                    if (rbuf->buf[n] != 'a') {
//                        ngx_log_error(NGX_LOG_INFO, log, 0, "ipc: rbuf[%7d] == %d '%c'", n, rbuf->buf[n], rbuf->buf[n]);
//                    }
//                }
                ngx_str_t t;
                t.data = rbuf->buf;
                t.len = rbuf->header.size;

//                t.data = ngx_alloc(rbuf->header.size, log);
//                t.len = rbuf->header.size;
//                if (t.data == NULL) {
//                    ngx_log_error(NGX_LOG_INFO, log, 0, "\t\tipc: rbuf handler nomem");
//                    reset_readbuf(rbuf);
//                    return NGX_OK;
//                }
//                ngx_memcpy(t.data, rbuf->buf, rbuf->header.size);

//                reset_readbuf(rbuf);

                ipc_proc->ipc->handler(rbuf->header.slot, rbuf->header.module, &t);
                ngx_ipc_reset_readbuf(rbuf);
                return NGX_OK;
            }
        }
    }
    ngx_log_error(NGX_LOG_CRIT, ngx_cycle->log, 0,
                  "ipc: ipc_read() reached end of function");
    // unreachable
    return NGX_OK;
}

static void ngx_ipc_read_handler(ngx_event_t *ev) {
    //copypaste from os/unix/ngx_process_cycle.c (ngx_channel_handler)
    ngx_int_t          rc;
    ngx_connection_t  *c;
    ipc_process_t     *ipc_proc;

    if (ev->timedout) {
        ev->timedout = 0;
        return;
    }
    c = ev->data;
    ipc_proc = &((ipc_t *)c->data)->process[ngx_process_slot];

    while (1) {
//        ngx_log_error(NGX_LOG_INFO, ev->log, 0, "\tipc: READ rbuf");
        rc = ngx_ipc_read(ipc_proc, &ipc_proc->rbuf, ev->log);
        if (rc == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, ev->log, 0, "\tipc: READ NGX_ERROR");
            break;
        } else if (rc == NGX_OK) {
//            ngx_log_error(NGX_LOG_INFO, ev->log, 0, "\tipc: READ NGX_OK");
            continue;
        } else if (rc == NGX_AGAIN) {
//            ngx_log_error(NGX_LOG_INFO, ev->log, 0, "\tipc: READ NGX_AGAIN");
            break;
        } else {
//            ngx_log_error(NGX_LOG_INFO, ev->log, 0, "\tipc: READ WTF");
            break;
        }
    }
//    ngx_log_error(NGX_LOG_INFO, ev->log, 0, "\tipc: READ done");
}

static ngx_int_t ipc_send_msg(ipc_t *ipc, ngx_int_t slot, ngx_int_t module_index, ngx_str_t *data) {
    ngx_int_t           i;
    u_char             *c;
    ipc_msg_link_t     *msg;
    ipc_process_t      *proc = &ipc->process[slot];
    ipc_writebuf_t     *wb = &proc->wbuf;
    size_t              msg_size = 0;

    ngx_str_t           empty = ngx_null_string;
    if (!data) {
        data = &empty;
    }

    if (slot == ngx_process_slot) {
        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                      "ipc: message %d->%d md=%d len=%ui",
                     ngx_process_slot, slot, module_index, data->len);
        ipc->handler(slot, module_index, data);
        return NGX_OK;
    }

    if (!proc->active) {
        return NGX_ERROR;
    }

    if ((msg = ngx_alloc(sizeof(ipc_msg_link_t), ngx_cycle->log)) == NULL) {
        return NGX_ERROR;
    }
    msg->next = NULL;
    msg->sent = 0;

    msg_size = IPC_HEADER_LEN + data->len;
    if ((msg->buf.data = ngx_alloc(msg_size, ngx_cycle->log)) == NULL) {
        ngx_free(msg);
        return NGX_ERROR;
    }

    //TODO: rewrite using macros or function

    c = msg->buf.data;
    for (i = 0; i < (int)sizeof(slot); i++) {
        int shift = 8 * (sizeof(slot) - i - 1);
        *c = (slot >> shift) & 0xFF;
        c++;
    }
    for (i = 0; i < (int)sizeof(module_index); i++) {
        int shift = 8 * (sizeof(module_index) - i - 1);
        *c = (module_index >> shift) & 0xFF;
        c++;
    }
    for (i = 0; i < (int)sizeof(data->len); i++) {
        int shift = 8 * (sizeof(data->len) - i - 1);
        *c = (data->len >> shift) & 0xFF;
        c++;
    }

//    ngx_memcpy(msg->buf.data + IPC_HEADER_LEN, data->data, data->len);
    for (i = 0; i < (int)data->len; i++) {
        msg->buf.data[i + IPC_HEADER_LEN] = data->data[i];
    }

    msg->buf.len = msg_size;

//    for (i = 0; i < (int)IPC_HEADER_LEN + (int)data->len; i++) {
//        if (msg->buf.data[i] != 'a') {
//            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "write_msg buf.data[%5d] == '%c' (%d)",
//                          i, msg->buf.data[i], (int)msg->buf.data[i]);
//        }
//    }

    if (wb->n == 0) {
        wb->head = wb->tail = msg;
    } else {
        wb->tail->next = msg;
        wb->tail = msg;
        msg->next = NULL;
    }
    wb->n++;

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                  "ipc: message %d->%d md=%d len=%ui qsz=%d",
                  ngx_process_slot, slot, module_index, data->len, wb->n);

    ngx_ipc_write_handler(proc->c->write);

    return NGX_OK;
}

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
        ngx_ipc_init(ipc);
        ipc->handler = ngx_ipc_msg_handler;
    }
    ngx_ipc_open(ipc, cycle, ccf->worker_processes, NULL);

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
    ngx_ipc_close(ipc, cycle);
}

static void ngx_ipc_exit_master(ngx_cycle_t *cycle) {
    ngx_ipc_close(ipc, cycle);
    shm_free(shm, shdata);
    shm_destroy(shm);
}
