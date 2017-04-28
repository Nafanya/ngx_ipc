#include <ngx_channel.h>
#include <assert.h>

#include "ngx_ipc_core.h"

#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "ipc: " fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "ipc: " fmt, ##args)


static void ipc_write_handler(ngx_event_t *ev);
static ngx_int_t reset_readbuf(ipc_readbuf_t *b);
static ngx_int_t ipc_free_buffered_msg(ipc_msg_link_t *msg_link);
static void ipc_read_handler(ngx_event_t *ev);

ngx_int_t ipc_init(ipc_t *ipc) {
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
        reset_readbuf(&proc->rbuf);
    }
    return NGX_OK;
}

static ngx_int_t reset_readbuf(ipc_readbuf_t *b) {
    ngx_memzero(&b->header, sizeof(b->header));
    b->header.bp = 0;

    b->complete = 0;
    b->bp = 0;

    if (b->buf) {
        ngx_free(b->buf);
        b->buf = NULL;
    }

    return NGX_OK;
}

ngx_int_t ipc_set_handler(ipc_t *ipc, void (*msg_handler)(ngx_int_t, ngx_int_t, size_t, u_char *)) {
    ipc->handler = msg_handler;
    return NGX_OK;
}

static void ipc_try_close_fd(ngx_socket_t *fd) {
    if (*fd != NGX_INVALID_FILE) {
        ngx_close_socket(*fd);
        *fd = NGX_INVALID_FILE;
    }
}

ngx_int_t ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers,
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
            ipc_try_close_fd(&socks[0]);
            ipc_try_close_fd(&socks[1]);
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
                ipc_try_close_fd(&socks[0]);
                ipc_try_close_fd(&socks[1]);
                return NGX_ERROR;
            }
        }
        proc->active = 1;

        s++;
    }

    return NGX_OK;
}

ngx_int_t ipc_close(ipc_t *ipc, ngx_cycle_t *cycle) {
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

        for (cur = proc->wbuf.head; cur != NULL; cur = cur_next) {
            cur_next = cur->next;
            ipc_free_buffered_msg(cur);
        }

        if (proc->rbuf.buf) {
            free(proc->rbuf.buf);
            ngx_memzero(&proc->rbuf, sizeof(proc->rbuf));
        }

        ipc_try_close_fd(&proc->pipe[0]);
        ipc_try_close_fd(&proc->pipe[1]);
        ipc->process[i].active = 0;
    }
    return NGX_OK;
}

ngx_int_t ipc_register_worker(ipc_t *ipc, ngx_cycle_t *cycle) {
    int                    i;
    ngx_connection_t      *c;
    ipc_process_t         *proc;

    for (i = 0; i< NGX_MAX_PROCESSES; i++) {

        proc = &ipc->process[i];

        if (!proc->active) continue;

        assert(proc->pipe[0] != NGX_INVALID_FILE);
        assert(proc->pipe[1] != NGX_INVALID_FILE);

        if (i == ngx_process_slot) {
            c = ngx_get_connection(proc->pipe[0], cycle->log);
            c->data = ipc;

            c->read->handler = ipc_read_handler;
            c->read->log = cycle->log;
            c->write->handler = NULL;

            ngx_add_event(c->read, NGX_READ_EVENT, 0);
            proc->c=c;
        } else {
            c = ngx_get_connection(proc->pipe[1], cycle->log);

            c->data = proc;

            c->read->handler = NULL;
            c->write->log = cycle->log;
            c->write->handler = ipc_write_handler;

            proc->c=c;
        }
    }
    return NGX_OK;
}

static ngx_int_t ipc_write_buffered_msg(ngx_socket_t fd, ipc_msg_link_t *msg) {
    ssize_t      n;
    ngx_int_t    err;
    uint16_t     unsent;
    u_char      *data;

    unsent = msg->buf.len - msg->sent;
    data = msg->buf.data + msg->sent;
    n = write(fd, data, unsent);
    if (n == -1) {
        err = ngx_errno;
        if (err == NGX_EAGAIN) {
            return NGX_AGAIN;
        }

        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err, "write() failed");
        return NGX_ERROR;
    } else if (n < unsent) {
        msg->sent += n;
        return NGX_AGAIN;
    }

    return NGX_OK;
}

static ngx_int_t ipc_free_buffered_msg(ipc_msg_link_t *msg_link) {
    ngx_free(msg_link);
    return NGX_OK;
}

static void ipc_write_handler(ngx_event_t *ev) {
    ngx_connection_t *c = ev->data;
    ngx_socket_t      fd = c->fd;

    ipc_process_t    *proc = (ipc_process_t *) c->data;
    ipc_msg_link_t   *cur;

    ngx_int_t         rc;
    uint8_t           aborted = 0;

    while ((cur = proc->wbuf.head) != NULL) {
        rc = ipc_write_buffered_msg(fd, cur);

        if (rc == NGX_AGAIN) {
            aborted = 1;
            break;
        } else if (rc == NGX_OK) {
            proc->wbuf.head = cur->next;
            if (proc->wbuf.tail == cur) {
                proc->wbuf.tail = NULL;
            }
            ipc_free_buffered_msg(cur);
        } else {
            aborted = 1;
            break;
        }
    }

    if (aborted) {
        ngx_handle_write_event(c->write, 0);
    }
}

static ngx_int_t ipc_read(ipc_process_t *ipc_proc, ipc_readbuf_t *rbuf, ngx_log_t *log) {
    ngx_int_t       n;
    ngx_err_t       err;
    ngx_socket_t    s = ipc_proc->c->fd;
    u_char         *p;

    while (!rbuf->header.complete) {
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
                p = rbuf->header.buf;
                rbuf->header.slot   = (*p << 24) + (*(p+1) << 16) + (*(p+2) << 8) + *(p+3);
                p += 4;
                rbuf->header.module = (*p << 24) + (*(p+1) << 16) + (*(p+2) << 8) + *(p+3);
                p += 4;
                rbuf->header.size   = (*p << 24) + (*(p+1) << 16) + (*(p+2) << 8) + *(p+3);
                p += 4;
                rbuf->header.complete = 1;
                rbuf->bp = 0;
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
        n = read(s, rbuf->buf + rbuf->bp, rbuf->header.size - rbuf->bp);
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
            rbuf->bp += n;
            if (rbuf->bp == rbuf->header.size) {
                ipc_proc->ipc->handler(rbuf->header.slot, rbuf->header.module, rbuf->header.size, rbuf->buf);
                reset_readbuf(rbuf);
                return NGX_OK;
            }
        }
    }
    // unreachable
    return NGX_OK;
}

static void ipc_read_handler(ngx_event_t *ev) {
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
        rc = ipc_read(ipc_proc, &ipc_proc->rbuf, ev->log);
        switch (rc) {
        case NGX_ERROR:
            ngx_log_error(NGX_LOG_ERR, ev->log, 0, "ipc: ipc_read parsing buf failed");
            break;
        case NGX_OK:
            continue;
            break;
        case NGX_AGAIN:
            return;
        default:
            return;
        }
    }
}

ngx_int_t ipc_send_msg(ipc_t *ipc, ngx_int_t slot, ngx_int_t module_index, ngx_str_t *data) {
  
    ipc_msg_link_t     *msg;
    ipc_process_t      *proc = &ipc->process[slot];
    ipc_writebuf_t     *wb = &proc->wbuf;
    size_t              msg_size = 0;

    ngx_str_t           empty = ngx_null_string;
    if (!data) {
        data = &empty;
    }

    if (slot == ngx_process_slot) {
        ipc->handler(slot, module_index, data->len, data->data);
        return NGX_OK;
    }

    if (!proc->active) {
        return NGX_ERROR;
    }

    msg_size += IPC_HEADER_LEN + data->len;
    if ((msg = ngx_alloc(sizeof(*msg) + msg_size, ngx_cycle->log)) == NULL) {
        return NGX_ERROR;
    }
    msg->next = NULL;
    msg->sent = 0;
    msg->buf.data = (u_char *)&msg[1];

    msg->buf.data[0] = (slot >> 24) & 0xFF;
    msg->buf.data[1] = (slot >> 16) & 0xFF;
    msg->buf.data[2] = (slot >> 8)  & 0xFF;
    msg->buf.data[3] = (slot)       & 0xFF;

    msg->buf.data[4] = (module_index >> 24) & 0xFF;
    msg->buf.data[5] = (module_index >> 16) & 0xFF;
    msg->buf.data[6] = (module_index >> 8)  & 0xFF;
    msg->buf.data[7] = (module_index)       & 0xFF;

    msg->buf.data[8]  = (data->len >> 24) & 0xFF;
    msg->buf.data[9]  = (data->len >> 16) & 0xFF;
    msg->buf.data[10] = (data->len >> 8)  & 0xFF;
    msg->buf.data[11] = (data->len)       & 0xFF;

    ngx_memcpy(msg->buf.data + 12, data->data, data->len);

    if (wb->tail != NULL) {
        wb->tail->next = msg;
    }
    wb->tail = msg;
    if (wb->head == NULL) {
        wb->head = msg;
    }
    ipc_write_handler(proc->c->write);

    return NGX_OK;
}

