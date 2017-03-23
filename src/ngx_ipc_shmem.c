#include <ngx_http.h>
#include <nginx.h>

#include "ngx_ipc_shmem.h"

#define DEBUG_SHM_ALLOC 0
#define SHPOOL(shmem) ((ngx_slab_pool_t *)(shmem)->zone->shm.addr)

#define DEBUG_LEVEL NGX_LOG_DEBUG
#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "SHMEM(%i):" fmt, memstore_slot(), ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "SHMEM(%i):" fmt, memstore_slot(), ##args)

//shared memory
shmem_t *shm_create(ngx_str_t *name, ngx_module_t *module, ngx_conf_t *cf, size_t shm_size,
                    ngx_int_t (*init)(ngx_shm_zone_t *, void *), void *privdata) {
    ngx_shm_zone_t    *zone;
    shmem_t           *shm;

    shm_size = ngx_align(shm_size, ngx_pagesize);
    if (shm_size < 8 * ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "The push_max_reserved_memory value must be at least %udKiB",
                           (8 * ngx_pagesize) >> 10);
        shm_size = 8 * ngx_pagesize;
    }
    ngx_conf_log_error(NGX_LOG_INFO, cf, 0,
                       "Using %udKiB of shared memory for nchan", shm_size >> 10);

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

ngx_int_t shm_init(shmem_t *shm) {
    ngx_slab_pool_t    *shpool = SHPOOL(shm);
#if (DEBUG_SHM_ALLOC == 1)
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                  "nchan_shpool start %p size %i", shpool->start,
                  (u_char *)shpool->end - (u_char *)shpool->start);
#endif
    ngx_slab_init(shpool);

    return NGX_OK;
}

ngx_int_t shm_reinit(shmem_t *shm) {
    ngx_slab_pool_t    *shpool = SHPOOL(shm);
    ngx_slab_init(shpool);

    return NGX_OK;
}

void shmtx_lock(shmem_t *shm) {
    ngx_shmtx_lock(&SHPOOL(shm)->mutex);
}

void shmtx_unlock(shmem_t *shm) {
    ngx_shmtx_unlock(&SHPOOL(shm)->mutex);
}

ngx_int_t shm_destroy(shmem_t *shm) {
    ngx_free(shm);

    return NGX_OK;
}

void *shm_alloc(shmem_t *shm, size_t size, const char *label) {
    void         *p;
#if FAKESHARD
    p = ngx_alloc(size, ngx_cycle->log);
#else
    p = ngx_slab_alloc(SHPOOL(shm), size);
#endif
    if (p == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "shpool alloc failed");
    }

#if (DEBUG_SHM_ALLOC == 1)
    if (p != NULL) {
        ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                      "shpool alloc addr %p size %ui label %s", p, size,
                      label == NULL ? "none" : label);
    }
#endif
    return p;
}

void *shm_calloc(shmem_t *shm, size_t size, const char *label) {
    void *p = shm_alloc(shm, size, label);
    if (p != NULL) {
        ngx_memzero(p, size);
    }
    return p;
}

void shm_free(shmem_t *shm, void *p) {
#if FAKESHARD
    ngx_free(p);
#else
    ngx_slab_free(SHPOOL(shm), p);
#endif
#if (DEBUG_SHM_ALLOC == 1)
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "shpool free addr %p", p);
#endif
}

