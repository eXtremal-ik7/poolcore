#pragma once

#include "blockmaker/btc.h"
#include "blockmaker/merkleTree.h"
#include "poolcore/backend.h"
#include "rapidjson/document.h"
#include "loguru.hpp"
#include "asyncio/socket.h"


using ListenerCallback = std::function<void(socketTy, HostAddress, void*)>;

struct ListenerContext {
   ListenerCallback Callback;
  void *Arg;
};

static void listenerAcceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socket, void *arg)
{
  if (status == aosSuccess) {
    ListenerContext *ctx = static_cast<ListenerContext*>(arg);
    ctx->Callback(socket, address, ctx->Arg);
  }

  aioAccept(object, 0, listenerAcceptCb, arg);
}

static void createListener(asyncBase *base, uint16_t port, ListenerCallback callback, void *arg)
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(port);
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(hSocket);
  if (socketBind(hSocket, &address) != 0) {
    LOG_F(ERROR, "cannot bind port: %i", port);
    exit(1);
  }

  if (socketListen(hSocket) != 0) {
    LOG_F(ERROR, "listen error: %i", port);
    exit(1);
  }

  aioObject *object = newSocketIo(base, hSocket);

  ListenerContext *context = new ListenerContext;
  context->Callback = callback;
  context->Arg = arg;
  aioAccept(object, 0, listenerAcceptCb, context);
}
