#ifndef __IO_REQ_H__
#define __IO_REQ_H__

typedef struct io_req {
    uint64_t offset;
    size_t len;
    struct iovec *iovs;
    int iovcnt;
    bool is_read;
    void *bdev_io;
}io_req_t;


#endif
