// Based on grpc async sample.

#include "common.h"
#include <shared_mutex>

int g_num_workers = 2;

class ServerImpl final {
public:
  ServerImpl(int buffer_size) : global_buf_(new uint8_t[buffer_size]) {}
  ~ServerImpl() {
    for(auto& t : workers)   t.join();

    grpc_server_->Shutdown();

    for (int i = 0; i < g_num_workers; i++) {
      server_cq_vec_[i]->Shutdown();
    }
  }

  void Run() {
    std::string server_address("0.0.0.0:50051");

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterAsyncGenericService(&service_);
    for (int i = 0; i < g_num_workers; i++) {
        server_cq_vec_.emplace_back(builder.AddCompletionQueue());
    }
    grpc_server_ = builder.BuildAndStart();
    std::cout << "Server listening on " << server_address << std::endl;

    for (int i=0;i<g_num_workers;i++) {
        workers.emplace_back(thread(&ServerImpl::HandleRpcs, this, i));
    }

  }

private:
  class CallData {
  public:
    CallData(ServerImpl *server, AsyncGenericService *service,
             ServerCompletionQueue *cq)
        : server_impl(server), async_service(service), server_cq(cq),
          status(CREATE), stream(&server_ctx) {
      Proceed();
    }

    void Proceed() {
      if (status == CREATE) {
        status = PROCESS;

        async_service->RequestCall(&server_ctx, &stream, server_cq, server_cq,
                                   this);
      } else if (status == PROCESS) {
        new CallData(server_impl, async_service, server_cq);

        stream.Read(&srv_recv_buffer, this);

        status = REPLY;
      } else if (status == REPLY) {
        rpc_payload_t request_payload{}, response_payload{};
        ParseFromByteBuffer(&srv_recv_buffer, &request_payload, true);

        if (server_ctx.method() == kReadMethodName) {
            response_payload.iovs = (struct iovec*)calloc(request_payload.h.iovcnt, sizeof(struct iovec));
            server_impl->ReadBuffer(&request_payload, &response_payload);
            response_payload.h.iovcnt = request_payload.h.iovcnt;
            response_payload.h.type = req_type::read_response;
        } else {
            response_payload.iovs = NULL;
            server_impl->WriteBuffer(&request_payload);
            response_payload.h.type = req_type::write_response;
            response_payload.h.iovcnt = 0;
        }

        SerializeToByteBuffer(srv_send_buffer, &response_payload);
        if (response_payload.iovs) {
            free(response_payload.iovs);
        }
        stream.Write(srv_send_buffer, this);
        status = FINISH;
      } else if (status == FINISH) {
        stream.Finish(Status::OK, this);
        status = DESTROY;
      } else {
        assert(status == DESTROY);
        delete this;
      }
    }

  private:
    AsyncGenericService *async_service;
    ServerCompletionQueue *server_cq;
    GenericServerContext server_ctx;
    bool ok;
    void *got_tag;

    ByteBuffer srv_recv_buffer;
    ByteBuffer srv_send_buffer;

    GenericServerAsyncReaderWriter stream;
    ServerImpl *server_impl;

    enum CallStatus { CREATE, PROCESS, REPLY, FINISH, DESTROY };
    CallStatus status;
  };

  void HandleRpcs(int i) {
    new CallData(this, &service_, server_cq_vec_[i].get());
    bool ok;
    while (true) {
      server_cq_vec_[i]->Next(&tag, &ok);
      assert(ok);
      static_cast<CallData *>(tag)->Proceed();
    }
  }

    void ReadBuffer(rpc_payload_t *request, rpc_payload_t *response) {
        uint8_t *src = global_buf_ + request->h.offset;
        for (int i=0;i<request->h.iovcnt; i++) {
            response->iovs[i].iov_base =  src;
            response->iovs[i].iov_len = request->iovs[i].iov_len;
            src += request->iovs[i].iov_len;
        }
    }

    void WriteBuffer(rpc_payload_t *request) {
        uint8_t *dst = global_buf_ + request->h.offset;
        for (int i=0;i<request->h.iovcnt; i++) {
            memcpy(dst, request->iovs[i].iov_base, request->iovs[i].iov_len);
            dst += request->iovs[i].iov_len;
        }
    }

  vector<std::unique_ptr<ServerCompletionQueue>> server_cq_vec_;
  AsyncGenericService service_;
  std::unique_ptr<Server> grpc_server_;
  uint8_t *global_buf_;
  std::mutex mutex;
  vector<thread> workers;
};

int main(int argc, char **argv) {
  int size_mb = std::stoi(argv[1]) * 1024 * 1024;
  g_num_workers = std::stoi(argv[2]);
  ServerImpl server(size_mb);
  server.Run();

  return 0;
}
