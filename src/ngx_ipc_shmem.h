#ifndef NGX_IPC_SHMEM_H
#define NGX_IPC_SHMEM_H

#include <ngx_core.h>

#define ngx_ipc_shpool(shmem) ((ngx_slab_pool_t *)(shmem)->zone->shm.addr)
#define NGX_IPC_DEBUG_SHM 1

typedef struct ngx_ipc_shmem_s     ngx_ipc_shmem_t;

struct ngx_ipc_shmem_s {
    ngx_shm_zone_t        *zone;
};


ngx_ipc_shmem_t *shm_create(ngx_str_t *name, ngx_module_t *module, ngx_conf_t *cf, size_t shm_size,
                    ngx_int_t (*init)(ngx_shm_zone_t *, void *), void *privdata);

ngx_int_t shm_init(ngx_ipc_shmem_t *shm);
ngx_int_t shm_reinit(ngx_ipc_shmem_t *shm);
ngx_int_t shm_destroy(ngx_ipc_shmem_t *shm);

void *shm_alloc(ngx_ipc_shmem_t *shm, size_t size, const char *label);
void *shm_calloc(ngx_ipc_shmem_t *shm, size_t size, const char *label);
void shm_free(ngx_ipc_shmem_t *shm, void *p);

void shmtx_lock(ngx_ipc_shmem_t *shm);
void shmtx_unlock(ngx_ipc_shmem_t *shm);

#endif //NGX_IPC_SHMEM_H
