#ifndef __COMMON_H_
#define __COMMON_H_

#include <sys/uio.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <google/protobuf/message_lite.h>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/proto_utils.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/proto_buffer_writer.h>
#include <grpcpp/support/slice.h>
#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/completion_queue.h>
#include "io_req.h"

using namespace std;
using namespace grpc;

#define COOKIE 0xABC123
enum class req_type {read_request = 1, read_response = 2, write_request=3, write_response=4 };
typedef struct rpc_payload {
  struct header {
    req_type type = req_type::read_request;
    uint64_t offset = 0;
    int iovcnt;
    uint64_t cookie = COOKIE;
  }h;
  struct iovec *iovs;
}rpc_payload_t;

const std::string server_address = "0.0.0.0:50051";
const std::string kWriteMethodName = "sds.grpc.io.write";
const std::string kReadMethodName = "sds.grpc.io.read";

void SerializeToByteBuffer(grpc::ByteBuffer& cli_byte_buf, rpc_payload_t *payload);
bool ParseFromByteBuffer(ByteBuffer* buffer, rpc_payload_t *payload, bool server);

// On server side we use same malloced buffer pointer in rpc payload iovec.
// On client side we use same iovec pointer from bdev in rpc payload iovec.
void SerializeToByteBuffer(grpc::ByteBuffer& cli_byte_buf, rpc_payload_t *payload) {
    vector< grpc::Slice> slices;
    int header_len = sizeof(payload->h);
    assert(payload->h.cookie == COOKIE);

    slices.emplace_back(&payload->h, header_len);
    if (payload->iovs) {
        for (int i=0;i<payload->h.iovcnt;i++) {
            slices.emplace_back(&payload->iovs[i].iov_len, sizeof(int));
            slices.emplace_back(payload->iovs[i].iov_base, payload->iovs[i].iov_len, Slice::STATIC_SLICE);
        }
    }

    cli_byte_buf.Clear();
    grpc::ByteBuffer tmp(slices.data(), slices.size());
    cli_byte_buf.Swap(&tmp);
}

bool ParseFromByteBuffer(ByteBuffer* buffer, rpc_payload_t *payload, bool server) {
  Slice slice;
  (void)buffer->DumpToSingleSlice(&slice);

  const uint8_t *p = slice.begin();
  int header_len = sizeof(payload->h);
  memcpy(&payload->h, p, header_len);
  assert(payload->h.cookie == COOKIE);
  p += sizeof(payload->h);

  // On server side we dont copy the buffers.
  if (server) {
      payload->iovs = (struct iovec*)calloc(payload->h.iovcnt, sizeof(struct iovec));
  }

  struct iovec *iovs = payload->iovs;
  for (int i = 0; i < payload->h.iovcnt; i++) {
      int len = *reinterpret_cast<const int*>(p);
      iovs[i].iov_len = len;
      p += sizeof(int);
      if (server) {
          iovs[i].iov_base = const_cast<void*>(reinterpret_cast<const void*>(p));
      } else {
        if (iovs[i].iov_base) {
          memcpy(iovs[i].iov_base, p, len);
        }
      }
      p += len;
  }
  return true;
}

#endif
