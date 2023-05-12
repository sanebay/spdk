#ifndef SPDK_BDEV_HS_IO_TARGET_H
#define SPDK_BDEV_HS_IO_TARGET_H

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "bdev_hs.h"
#include "io_req.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*io_complete_cb_t)(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status);

int io_target_read(void *ctx, io_req_t *req);
int io_target_write(void *ctx, io_req_t *req);
void *create_io_target(struct hs_bdev_opts *opts, io_complete_cb_t cb);
void destroy_io_target(void *ctx);

#ifdef __cplusplus
}
#endif


#endif
