#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/completion_queue.h>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "spdk/log.h"

#include "io_target.h"
#include "common.h"
#include "io_req.h"

using namespace std;
using namespace grpc;
const int g_num_workers = 2;

class IOTarget {
public:
    IOTarget(struct hs_bdev_opts *opts, io_complete_cb_t cb) : m_opts(*opts), io_complete_cb(cb) {}
    virtual ~IOTarget() = default;
    virtual int read(io_req_t* req) = 0;
    virtual int write(io_req_t* req) = 0;
protected:
    struct hs_bdev_opts m_opts;
    io_complete_cb_t io_complete_cb;
};

class MemoryTarget : public IOTarget {
public:
    MemoryTarget(struct hs_bdev_opts *opts, io_complete_cb_t cb);
    ~MemoryTarget();
    int read(io_req_t* req) override;
    int write(io_req_t* req) override;
private:
    uint8_t *malloc_buf;
};

struct RpcData {
    ByteBuffer cli_recv_buffer;
    Status recv_status;
    unique_ptr<GenericClientAsyncResponseReader> call;
    ClientContext cli_ctx;
    bool is_read;
    rpc_payload_t payload;
    void *bdev_io;
};

class GrpcTarget : public IOTarget {
public:
    GrpcTarget(struct hs_bdev_opts *opts, io_complete_cb_t cb);
    ~GrpcTarget();
    int read(io_req_t* req) override;
    int write(io_req_t* req) override;
private:
    int SendRpc(io_req_t* req);
    void ClientLoop();
private:
    string m_server_address;
    grpc::GenericStub m_generic_stub;
    CompletionQueue m_cli_cq;
    vector<thread> m_workers;
    bool run_loop = false;
};

class DpdkTarget : public IOTarget {
public:
    DpdkTarget(struct hs_bdev_opts *opts, io_complete_cb_t cb);
    ~DpdkTarget();
    int read(io_req_t* req) override;
    int write(io_req_t* req) override;
};

MemoryTarget::MemoryTarget(struct hs_bdev_opts *opts, io_complete_cb_t cb) : IOTarget(opts, cb) {
    malloc_buf = (uint8_t*)spdk_zmalloc(m_opts.block_size * m_opts.num_blocks, m_opts.block_size, NULL,
                                  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
    assert(malloc_buf);
}

MemoryTarget::~MemoryTarget() { spdk_free(malloc_buf); }

int MemoryTarget::read(io_req_t* req) {
    uint8_t *src = malloc_buf + req->offset;
    for(int i=0;i < req->iovcnt; i++) {
        memcpy(req->iovs[i].iov_base, src, req->iovs[i].iov_len);
        src += req->iovs[i].iov_len;
    }

    io_complete_cb(reinterpret_cast<struct spdk_bdev_io*>(req->bdev_io), SPDK_BDEV_IO_STATUS_SUCCESS);
    return 0;
}

int MemoryTarget::write(io_req_t* req) {
    uint8_t *dst = malloc_buf + req->offset;
    for(int i=0;i < req->iovcnt; i++) {
        memcpy(dst, req->iovs[i].iov_base, req->iovs[i].iov_len);
        dst += req->iovs[i].iov_len;
    }
    io_complete_cb(reinterpret_cast<struct spdk_bdev_io*>(req->bdev_io), SPDK_BDEV_IO_STATUS_SUCCESS);
    return 0;
}

GrpcTarget::GrpcTarget(struct hs_bdev_opts *opts, io_complete_cb_t cb)
    : IOTarget(opts, cb), m_server_address(opts->host),
      m_generic_stub (grpc::GenericStub(
                          grpc::CreateChannel(m_server_address, InsecureChannelCredentials()))) {

    for(int i=0;i<g_num_workers;i++) {
        m_workers.emplace_back(thread([this]() { ClientLoop(); }));
    }
}

GrpcTarget::~GrpcTarget() {
    m_cli_cq.Shutdown();
    for (auto& t : m_workers) t.join();
}

void GrpcTarget::ClientLoop() {
  void *tag;
  bool ok;
  while (m_cli_cq.Next(&tag, &ok)) {
    assert(ok);
    RpcData *rpc_data = static_cast<RpcData *>(tag);
    spdk_bdev_io_status status;
    if (rpc_data->recv_status.ok()) {
      // We have the pointers to iovec and so we are directly copying.
      ParseFromByteBuffer(&rpc_data->cli_recv_buffer, &rpc_data->payload, false);
      status = SPDK_BDEV_IO_STATUS_SUCCESS;
    } else {
      cout << "Got error " << rpc_data->recv_status.error_code() << ":"
           << rpc_data->recv_status.error_message() << endl;
      status = SPDK_BDEV_IO_STATUS_FAILED;
    }

    io_complete_cb(reinterpret_cast<struct spdk_bdev_io*>(rpc_data->bdev_io), status);
    delete rpc_data;
  }
}

int GrpcTarget::SendRpc(io_req_t* req) {
    auto rpc_data = new RpcData; // todo mempool
    string method_name;
    req_type type;
    if (req->is_read) {
        type = req_type::read_request;
        method_name = kReadMethodName;
    } else {
        type = req_type::write_request;
        method_name = kWriteMethodName;
    }

    ByteBuffer cli_send_buffer;
    rpc_data->payload.h.type = type;
    rpc_data->payload.h.offset =  req->offset;
    rpc_data->payload.h.iovcnt = req->iovcnt;
    rpc_data->payload.iovs = req->iovs;
    rpc_data->bdev_io = req->bdev_io;

    SerializeToByteBuffer(cli_send_buffer, &rpc_data->payload);

    rpc_data->call = m_generic_stub.PrepareUnaryCall(&rpc_data->cli_ctx, method_name,
                                                      cli_send_buffer, &m_cli_cq);
    rpc_data->call->StartCall();
    rpc_data->call->Finish(&rpc_data->cli_recv_buffer, &rpc_data->recv_status, rpc_data);
    return 0;
}

int GrpcTarget::read(io_req_t* req) {
    return SendRpc(req);
}

int GrpcTarget::write(io_req_t* req) {
    return SendRpc(req);
}

int io_target_read(void *ctx, io_req_t *req) {
    assert(ctx);
    assert(req);
    return reinterpret_cast<IOTarget*>(ctx)->read(req);
}

int io_target_write(void *ctx, io_req_t *req) {
    assert(ctx);
    assert(req);
    return reinterpret_cast<IOTarget*>(ctx)->write(req);
}

void *create_io_target(struct hs_bdev_opts *opts, io_complete_cb_t cb) {
    IOTarget *target;
    if (opts->mode == 1) {
        target = new GrpcTarget(opts, cb);
        SPDK_NOTICELOG("Created grpc target\n");
    } else {
        target = new MemoryTarget(opts, cb);
        SPDK_NOTICELOG("Created in memory target\n");
    }

    return target;
}

void destroy_io_target(void *ctx) {
    delete reinterpret_cast<IOTarget *>(ctx);
}
