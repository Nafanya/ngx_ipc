#ifndef NGX_IPC_SHMEM_H
#define NGX_IPC_SHMEM_H

#include <ngx_core.h>


typedef struct ipc_shmem_s     shmem_t;

struct ipc_shmem_s {
    ngx_shm_zone_t        *zone;
};


shmem_t    *shm_create(ngx_str_t *name, ngx_module_t *module, ngx_conf_t *cf, size_t shm_size,
                       ngx_int_t (*init)(ngx_shm_zone_t *, void *), void *privdata);
ngx_int_t   shm_init(shmem_t *shm);
ngx_int_t   shm_reinit(shmem_t *shm);
ngx_int_t   shm_destroy(shmem_t *shm);
void       *shm_alloc(shmem_t *shm, size_t size, const char *label);
void       *shm_calloc(shmem_t *shm, size_t size, const char *label);
void        shm_free(shmem_t *shm, void *p);

void        shmtx_lock(shmem_t *shm);
void        shmtx_unlock(shmem_t *shm);

ngx_str_t  *shm_copy_immutable_string(shmem_t *shm, ngx_str_t *str);
void        shm_free_immutable_string(shmem_t *shm, ngx_str_t *str);
void        shm_verify_immutable_string(shmem_t *shm, ngx_str_t *str);

#endif //NGX_IPC_SHMEM_H
