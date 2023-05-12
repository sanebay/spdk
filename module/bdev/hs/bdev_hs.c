#include "spdk/stdinc.h"

#include "bdev_hs.h"

#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/string.h"

#include "spdk/log.h"
#include "io_target.h"

static int bdev_hs_count = 0;

struct bdev_hs {
  struct spdk_bdev bdev;
  char *hs_name;
  char *host;
  pthread_mutex_t mutex;
  struct spdk_thread *main_td;
  struct spdk_thread *destruct_td;
  uint32_t ch_count;
  int num_outstanding;

  TAILQ_ENTRY(bdev_hs) tailq;

  void *io_target;
};

struct bdev_hs_io_channel {
    struct bdev_hs *bdev;
};

struct bdev_hs_io {
    struct			spdk_thread *submit_td;
    enum			spdk_bdev_io_status status;
};

static void
bdev_hs_free(struct bdev_hs *hs)
{
    if (!hs) {
        return;
    }

    destroy_io_target(hs->io_target);

    free(hs->bdev.name);
    free(hs->hs_name);
    free(hs->host);
    free(hs);
}

static int bdev_hs_library_init(void);
static void bdev_hs_library_fini(void);

static int
bdev_hs_get_ctx_size(void)
{
    return sizeof(struct bdev_hs_io);
}

static struct spdk_bdev_module hs_if = {
    .name = "bdev_hs",
    .module_init = bdev_hs_library_init,
    .module_fini = bdev_hs_library_fini,
    .get_ctx_size = bdev_hs_get_ctx_size,

};
SPDK_BDEV_MODULE_REGISTER(hs, &hs_if)

static void
_bdev_hs_destruct_done(void *io_device)
{
    struct bdev_hs *hs = io_device;

    assert(hs != NULL);
    assert(hs->ch_count == 0);

    spdk_bdev_destruct_done(&hs->bdev, 0);
    bdev_hs_free(hs);
}

static void
bdev_hs_free_cb(void *io_device)
{
    struct bdev_hs *hs = io_device;

    /* The io device has been unregistered.  Send a message back to the
     * original thread that started the destruct operation, so that the
     * bdev unregister callback is invoked on the same thread that started
     * this whole process.
     */
    spdk_thread_send_msg(hs->destruct_td, _bdev_hs_destruct_done, hs);
}

static void
_bdev_hs_destruct(void *ctx)
{
    struct bdev_hs *hs = ctx;

    spdk_io_device_unregister(hs, bdev_hs_free_cb);
}

static int
bdev_hs_destruct(void *ctx)
{
    struct bdev_hs *hs = ctx;
    struct spdk_thread *td;

    if (hs->main_td == NULL) {
        td = spdk_get_thread();
    } else {
      td = hs->main_td;
    }

    /* Start the destruct operation on the hs bdev's
     * main thread.  This guarantees it will only start
     * executing after any messages related to channel
     * deletions have finished completing.  *Always*
     * send a message, even if this function gets called
     * from the main thread, in case there are pending
     * channel delete messages in flight to this thread.
     */
    assert(hs->destruct_td == NULL);
    hs->destruct_td = td;
    spdk_thread_send_msg(td, _bdev_hs_destruct, hs);

    /* Return 1 to indicate the destruct path is asynchronous. */
    return 1;
}

static void _bdev_hs_io_complete(void *ctx) {
    struct spdk_bdev_io *bdev_io = ctx;
    struct bdev_hs_io *hs_io = (struct bdev_hs_io *)bdev_io->driver_ctx;
    spdk_bdev_io_complete(bdev_io, hs_io->status);
}

static void bdev_hs_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status) {
    struct bdev_hs *bdev = (struct bdev_hs *)bdev_io->bdev->ctxt;
    struct bdev_hs_io *hs_io = (struct bdev_hs_io *)bdev_io->driver_ctx;
    hs_io->status = status;
    spdk_thread_exec_msg(bdev->main_td, _bdev_hs_io_complete, bdev_io);
}

static void
bdev_hs_start(void *ctx)
{
    struct spdk_bdev_io *bdev_io = ctx;
    struct bdev_hs *bdev = (struct bdev_hs *)bdev_io->bdev->ctxt;

    struct io_req req;
    req.bdev_io = bdev_io;
    req.iovs = bdev_io->u.bdev.iovs;
    req.iovcnt =  bdev_io->u.bdev.iovcnt;
    req.offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
    req.len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

    if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
        req.is_read = true;
        io_target_read(bdev->io_target, &req);
    } else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
        req.is_read = false;
        io_target_write(bdev->io_target, &req);
    } else {
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
    }
}

static void
bdev_hs_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
            bool success)
{
    struct bdev_hs *bdev = (struct bdev_hs *)bdev_io->bdev->ctxt;

    if (!success) {
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }

    spdk_thread_exec_msg(bdev->main_td, bdev_hs_start, bdev_io);
}

static void
bdev_hs_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
    struct spdk_thread *submit_td = spdk_io_channel_get_thread(ch);
    struct bdev_hs_io *hs_io = (struct bdev_hs_io *)bdev_io->driver_ctx;
    struct bdev_hs *bdev = (struct bdev_hs *)bdev_io->bdev->ctxt;

    hs_io->submit_td = submit_td;
    switch (bdev_io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        spdk_bdev_io_get_buf(bdev_io, bdev_hs_get_buf_cb,
                     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
        break;

    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_UNMAP:
    case SPDK_BDEV_IO_TYPE_FLUSH:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
        spdk_thread_exec_msg(bdev->main_td, bdev_hs_start, bdev_io);
        break;

    default:
        SPDK_ERRLOG("Unsupported IO type =%d\n", bdev_io->type);
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        break;
    }
}

static bool
bdev_hs_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
    switch (io_type) {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_UNMAP:
    case SPDK_BDEV_IO_TYPE_FLUSH:
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
        return true;

    default:
        return false;
    }
}

static void
bdev_hs_free_channel_resources(struct bdev_hs *bdev)
{
    assert(bdev != NULL);
    assert(bdev->main_td == spdk_get_thread());
    assert(bdev->ch_count == 0);

    bdev->main_td = NULL;
}

static int
bdev_hs_create_cb(void *io_device, void *ctx_buf)
{
    struct bdev_hs_io_channel *ch = ctx_buf;
    struct bdev_hs *bdev = io_device;

    ch->bdev = bdev;
    pthread_mutex_lock(&bdev->mutex);
    if (bdev->ch_count == 0) {
        assert(bdev->main_td == NULL);
        bdev->main_td = spdk_get_thread();
    }

    bdev->ch_count++;
    pthread_mutex_unlock(&bdev->mutex);

    return 0;
}

static void
_bdev_hs_destroy_cb(void *ctx)
{
    struct bdev_hs *bdev = ctx;

    pthread_mutex_lock(&bdev->mutex);
    assert(bdev->ch_count > 0);
    bdev->ch_count--;

    if (bdev->ch_count > 0) {
        /* A new channel was created between when message was sent and this function executed */
        pthread_mutex_unlock(&bdev->mutex);
        return;
    }

    bdev_hs_free_channel_resources(bdev);
    pthread_mutex_unlock(&bdev->mutex);
}

static void
bdev_hs_destroy_cb(void *io_device, void *ctx_buf)
{
    struct bdev_hs *bdev = io_device;
    struct spdk_thread *thread;

    pthread_mutex_lock(&bdev->mutex);
    assert(bdev->ch_count > 0);
    bdev->ch_count--;
    if (bdev->ch_count == 0) {
        assert(bdev->main_td != NULL);
        if (bdev->main_td != spdk_get_thread()) {
            /* The final channel was destroyed on a different thread
             * than where the first channel was created. Pass a message
             * to the main thread to unregister the poller. */
            bdev->ch_count++;
            thread = bdev->main_td;
            pthread_mutex_unlock(&bdev->mutex);
            spdk_thread_send_msg(thread, _bdev_hs_destroy_cb, bdev);
            return;
        }

        bdev_hs_free_channel_resources(bdev);
    }
    pthread_mutex_unlock(&bdev->mutex);
}

static struct spdk_io_channel *
bdev_hs_get_io_channel(void *ctx)
{
    struct bdev_hs *hs_bdev = ctx;

    return spdk_get_io_channel(hs_bdev);
}

static int bdev_hs_dump_info_json(void *ctx, struct spdk_json_write_ctx *w) {
  return 0;
}

static void bdev_hs_write_config_json(struct spdk_bdev *bdev,
                                       struct spdk_json_write_ctx *w) {}

static const struct spdk_bdev_fn_table hs_fn_table = {
    .destruct		= bdev_hs_destruct,
    .submit_request		= bdev_hs_submit_request,
    .io_type_supported	= bdev_hs_io_type_supported,
    .get_io_channel		= bdev_hs_get_io_channel,
    .dump_info_json		= bdev_hs_dump_info_json,
    .write_config_json	= bdev_hs_write_config_json,
};

int
bdev_hs_create(struct spdk_bdev **bdev, struct hs_bdev_opts *opts)
{
    struct bdev_hs *hs;
    int ret;

    hs = calloc(1, sizeof(struct bdev_hs));
    if (hs == NULL) {
        SPDK_ERRLOG("Failed to allocate bdev_hs struct\n");
        return -ENOMEM;
    }

    ret = pthread_mutex_init(&hs->mutex, NULL);
    if (ret) {
        SPDK_ERRLOG("Cannot init mutex on hs=%p\n", hs->bdev.name);
        free(hs);
        return ret;
    }

    hs->hs_name = strdup(opts->name);
    if (!hs->hs_name) {
        bdev_hs_free(hs);
        return -ENOMEM;
    }

    if (opts->name) {
        hs->bdev.name = strdup(opts->name);
    } else {
        hs->bdev.name = spdk_sprintf_alloc("hs-%d", bdev_hs_count);
    }
    if (!hs->bdev.name) {
        bdev_hs_free(hs);
        return -ENOMEM;
    }
    hs->bdev.product_name = "HS bdev client";
    bdev_hs_count++;

    hs->bdev.write_cache = 0;
    hs->bdev.blocklen = opts->block_size;
    hs->bdev.blockcnt = opts->num_blocks;
    hs->bdev.ctxt = hs;
    hs->bdev.fn_table = &hs_fn_table;
    hs->bdev.module = &hs_if;

    hs->io_target = create_io_target(opts, bdev_hs_io_complete);

    SPDK_NOTICELOG("Add %s hs bdev client \n", hs->bdev.name);

    spdk_io_device_register(hs, bdev_hs_create_cb,
                bdev_hs_destroy_cb,
                sizeof(struct bdev_hs_io_channel),
                "bdev_hs");
    ret = spdk_bdev_register(&hs->bdev);
    if (ret) {
        spdk_io_device_unregister(hs, NULL);
        bdev_hs_free(hs);
        return ret;
    }

    *bdev = &(hs->bdev);

    return ret;
}

void
bdev_hs_delete(const char *name, spdk_delete_hs_complete cb_fn, void *cb_arg)
{
    int rc;

    rc = spdk_bdev_unregister_by_name(name, &hs_if, cb_fn, cb_arg);
    if (rc != 0) {
        cb_fn(cb_arg, rc);
    }
}

static int
bdev_hs_library_init(void)
{
    return 0;
}

static void
bdev_hs_library_fini(void)
{
}

SPDK_LOG_REGISTER_COMPONENT(bdev_hs)
