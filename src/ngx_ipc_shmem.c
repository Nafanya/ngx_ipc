#include <ngx_http.h>

#include <ngx_ipc.h>
#include <ngx_ipc_shmem.h>

ngx_ipc_shmem_t *shm_create(ngx_str_t *name, ngx_module_t *module, ngx_conf_t *cf, size_t shm_size,
                    ngx_int_t (*init)(ngx_shm_zone_t *, void *), void *privdata) {
    ngx_shm_zone_t    *zone;
    ngx_ipc_shmem_t           *shm;

    shm_size = ngx_align(shm_size, ngx_pagesize);
    if (shm_size < 8 * ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "The ipc shm size must be at least %udKiB",
                           (8 * ngx_pagesize) >> 10);
        shm_size = 8 * ngx_pagesize;
    }
    ngx_conf_log_error(NGX_LOG_INFO, cf, 0,
                       "Using %udKiB of shared memory for ipc", shm_size >> 10);

    shm = ngx_alloc(sizeof(*shm), ngx_cycle->log);
    zone = ngx_shared_memory_add(cf, name, shm_size, module);
    if (zone == NULL || shm == NULL) {
        return NULL;
    }
    shm->zone = zone;

    zone->init = init;
    zone->data = (void *) 1;
    return shm;
}

ngx_int_t shm_init(ngx_ipc_shmem_t *shm) {
    ngx_slab_pool_t    *shpool = ngx_ipc_shpool(shm);

#if NGX_IPC_DEBUG_SHM
    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                  "ipc_shpool start %p size %i", shpool->start,
                  (u_char *)shpool->end - (u_char *)shpool->start);
#endif

    ngx_slab_init(shpool);

    return NGX_OK;
}

ngx_int_t shm_reinit(ngx_ipc_shmem_t *shm) {
    ngx_slab_pool_t *shpool = ngx_ipc_shpool(shm);

    ngx_slab_init(shpool);

    return NGX_OK;
}

void shmtx_lock(ngx_ipc_shmem_t *shm) {
    ngx_shmtx_lock(&ngx_ipc_shpool(shm)->mutex);
}

void shmtx_unlock(ngx_ipc_shmem_t *shm) {
    ngx_shmtx_unlock(&ngx_ipc_shpool(shm)->mutex);
}

ngx_int_t shm_destroy(ngx_ipc_shmem_t *shm) {
    ngx_free(shm);

    return NGX_OK;
}

void *shm_alloc(ngx_ipc_shmem_t *shm, size_t size, const char *label) {
    void         *p;
#if FAKESHARD
    p = ngx_alloc(size, ngx_cycle->log);
#else
    p = ngx_slab_alloc(ngx_ipc_shpool(shm), size);
#endif

    if (p == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "shpool alloc failed");
    }

#if NGX_IPC_DEBUG_SHM
    if (p != NULL) {
        ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                      "shpool alloc addr %p size %ui label %s", p, size,
                      label == NULL ? "none" : label);
    }
#endif

    return p;
}

void *shm_calloc(ngx_ipc_shmem_t *shm, size_t size, const char *label) {
    void *p = shm_alloc(shm, size, label);
    if (p != NULL) {
        ngx_memzero(p, size);
    }
    return p;
}

void shm_free(ngx_ipc_shmem_t *shm, void *p) {
#if FAKESHARD
    ngx_free(p);
#else
    ngx_slab_free(ngx_ipc_shpool(shm), p);
#endif
#if (DEBUG_SHM_ALLOC == 1)
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "shpool free addr %p", p);
#endif
}

