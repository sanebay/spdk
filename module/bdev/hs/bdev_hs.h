/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_HS_H
#define SPDK_BDEV_HS_H

#include "spdk/stdinc.h"

#include "spdk/bdev_module.h"

struct hs_bdev_opts {
    char *name;
    char *host;
    uint32_t num_blocks;
    uint32_t block_size;
    uint32_t mode;
};

typedef void (*spdk_delete_hs_complete)(void *cb_arg, int bdeverrno);

int bdev_hs_create(struct spdk_bdev **bdev, struct hs_bdev_opts *opts);

void bdev_hs_delete(const char *name, spdk_delete_hs_complete cb_fn, void *cb_arg);


#endif /* SPDK_BDEV_HS_H */
