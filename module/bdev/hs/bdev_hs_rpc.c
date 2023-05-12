/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_hs.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/log.h"

static void
free_hs_bdev_opts(struct hs_bdev_opts *opts)
{
    free(opts->name);
    free(opts->host);
}

static const struct spdk_json_object_decoder hs_bdev_opts_decoders[] = {
    {"name", offsetof(struct hs_bdev_opts, name), spdk_json_decode_string, true},
    {"host", offsetof(struct hs_bdev_opts, host), spdk_json_decode_string, true},
    {"num_blocks", offsetof(struct hs_bdev_opts, num_blocks), spdk_json_decode_uint32, true},
    {"block_size", offsetof(struct hs_bdev_opts, block_size), spdk_json_decode_uint32, true},
    {"mode", offsetof(struct hs_bdev_opts, mode), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_hs_create(struct spdk_jsonrpc_request *request,
            const struct spdk_json_val *params)
{
    struct hs_bdev_opts opts = {};
    struct spdk_json_write_ctx *w;
    struct spdk_bdev *bdev;
    int rc = 0;

    if (spdk_json_decode_object(params, hs_bdev_opts_decoders,
                    SPDK_COUNTOF(hs_bdev_opts_decoders),
                    &opts)) {
        SPDK_DEBUGLOG(bdev_hs, "spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                         "spdk_json_decode_object failed");
        goto cleanup;
    }

    rc = bdev_hs_create(&bdev, &opts);
    if (rc) {
        spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
        goto cleanup;
    }

    free_hs_bdev_opts(&opts);

    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_string(w, spdk_bdev_get_name(bdev));
    spdk_jsonrpc_end_result(request, w);
    return;

cleanup:
    free_hs_bdev_opts(&opts);
}
SPDK_RPC_REGISTER("bdev_hs_create", rpc_bdev_hs_create, SPDK_RPC_RUNTIME)

struct rpc_bdev_hs_delete {
    char *name;
};

static void
free_rpc_bdev_hs_delete(struct rpc_bdev_hs_delete *req)
{
    free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_hs_delete_decoders[] = {
    {"name", offsetof(struct rpc_bdev_hs_delete, name), spdk_json_decode_string},
};

static void
_rpc_bdev_hs_delete_cb(void *cb_arg, int bdeverrno)
{
    struct spdk_jsonrpc_request *request = cb_arg;

    if (bdeverrno == 0) {
        spdk_jsonrpc_send_bool_response(request, true);
    } else {
        spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
    }
}

static void
rpc_bdev_hs_delete(struct spdk_jsonrpc_request *request,
            const struct spdk_json_val *params)
{
    struct rpc_bdev_hs_delete req = {NULL};

    if (spdk_json_decode_object(params, rpc_bdev_hs_delete_decoders,
                    SPDK_COUNTOF(rpc_bdev_hs_delete_decoders),
                    &req)) {
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                         "spdk_json_decode_object failed");
        goto cleanup;
    }

    bdev_hs_delete(req.name, _rpc_bdev_hs_delete_cb, request);

cleanup:
    free_rpc_bdev_hs_delete(&req);
}
SPDK_RPC_REGISTER("bdev_hs_delete", rpc_bdev_hs_delete, SPDK_RPC_RUNTIME)
