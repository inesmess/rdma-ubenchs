#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <cassert>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>

#include "common3.h"

class Server : public RDMAPeer {
protected:
  rdma_cm_id *serverId;
  rdma_cm_id *clientId;
  ibv_mr *memReg;
  char *serverBuff;

  void HandleConnectRequest() {
    assert(eventChannel != NULL);
    assert(serverId != NULL);
    assert(event == NULL);
    D(std::cerr << "HandleConnectRequest\n");

    assert(rdma_get_cm_event(eventChannel, &event) == 0);
    assert(event->event == RDMA_CM_EVENT_CONNECT_REQUEST);

    D(std::cerr << "Received RDMA_CM_EVENT_CONNECT_REQUEST\n");

    clientId = (rdma_cm_id *) event->id;

    // create a prot domain for the client rdma device
    assert((protDomain = ibv_alloc_pd(clientId->verbs)) != NULL);
    assert((compQueue = ibv_create_cq(clientId->verbs, 32, 0, 0, 0)) != NULL);

    qpAttr.send_cq = qpAttr.recv_cq = compQueue;

    // queue pair
    assert(rdma_create_qp(clientId, protDomain, &qpAttr) == 0);
    assert(rdma_accept(clientId, &connParams) == 0);

    rdma_ack_cm_event(event);
  }

  void HandleConnectionEstablished() {
    assert(event != NULL);
    D(std::cerr << "HandleConnectionEstablished\n");

    assert(rdma_get_cm_event(eventChannel, &event) == 0);
    assert(event->event == RDMA_CM_EVENT_ESTABLISHED);
    rdma_ack_cm_event(event);
  }

  void HandleDisconnect() {
    assert(event != NULL);
    D(std::cerr << "HandleDisconnect\n");

    check_z(rdma_get_cm_event(eventChannel, &event));
    assert(event->event == RDMA_CM_EVENT_DISCONNECTED);
  }

public:
  Server() : serverId(NULL), clientId(NULL), memReg(NULL), serverBuff(NULL) {

    connParams = {};
    connParams.initiator_depth = 1;
    connParams.responder_resources = 1;
    qpAttr = {};
    qpAttr.cap.max_send_wr = 32;
    qpAttr.cap.max_recv_wr = 32;
    qpAttr.cap.max_send_sge = 1;
    qpAttr.cap.max_recv_sge = 1;
    qpAttr.cap.max_inline_data = 64;
    qpAttr.qp_type = IBV_QPT_RC;

    serverBuff = (char *) malloc(sizeof(char) * 256);
    strcpy(serverBuff, "HellO worlD!");

    assert((eventChannel = rdma_create_event_channel()) != NULL);
    assert(rdma_create_id(eventChannel, &serverId, NULL, RDMA_PS_TCP) == 0);

    sin = {};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    assert(rdma_bind_addr(serverId, (sockaddr *) &sin) == 0);
    assert(rdma_listen(serverId, 6) == 0);
  }

  virtual ~Server() {
    if (clientId)
      rdma_destroy_qp(clientId);

    if (compQueue)
      ibv_destroy_cq(compQueue);

    if (protDomain)
      ibv_dealloc_pd(protDomain);

    if (serverBuff)
      free(serverBuff);

    rdma_destroy_id(serverId);
    rdma_destroy_event_channel(eventChannel);
  }

  virtual void start(uint32_t entries) {
    assert(eventChannel != NULL);
    assert(serverId != NULL);

    HandleConnectRequest();
    HandleConnectionEstablished();

    SendTD send(protDomain, clientId->qp, entries);

    auto t0 = timer_start();
    send.Execute();

    WaitForCompletion();

    timer_end(t0);
    HandleDisconnect();
  }
};

// filtered server.
class FServer : Server {
public:
  // send only relevant data.
  void start(uint32_t entries) override {
    assert(eventChannel != NULL);
    assert(serverId != NULL);

    HandleConnectRequest();
    HandleConnectionEstablished();

    SendTDFiltered send(protDomain, clientId->qp, entries);
    send.filter(1);

    auto t0 = timer_start();
    send.Execute();

    WaitForCompletion();

    timer_end(t0);
    HandleDisconnect();
  }
};

class ServerSWrites : Server {
  RemoteRegInfo *info;

public:

  ServerSWrites() {
    info = new RemoteRegInfo();
  }

  ~ServerSWrites() {
    delete info;
  }

  void start(uint32_t entries) override {
    assert(eventChannel != NULL);
    assert(serverId != NULL);

    HandleConnectRequest();

    assert((memReg = ibv_reg_mr(protDomain, (void *) info, sizeof(RemoteRegInfo),
                                IBV_ACCESS_REMOTE_WRITE |
                                IBV_ACCESS_LOCAL_WRITE |
                                IBV_ACCESS_REMOTE_READ)) != NULL);

    // posting before receving RDMA_CM_EVENT_ESTABLISHED, otherwise
    // it fails saying there is no receive posted.
    PostWrRecv recvWr((uint64_t) info, sizeof(RemoteRegInfo), memReg->lkey, clientId->qp);
    recvWr.Execute();

    HandleConnectionEstablished();

    check_z(ibv_dereg_mr(memReg));
    // now setup the remote memory where we'll be able to write directly
    assert((memReg = ibv_reg_mr(protDomain, (void *) serverBuff, 256,
                                IBV_ACCESS_REMOTE_WRITE |
                                IBV_ACCESS_LOCAL_WRITE |
                                IBV_ACCESS_REMOTE_READ)) != NULL);

    D(std::cerr << "client addr=" << std::hex << info->addr);
    D(std::cerr << "\nclient rkey=" << std::dec << info->rKey);

    //WaitForCompletion();

    PostRDMAWrSend rdmaSend((uint64_t) serverBuff, 256, memReg->lkey, clientId->qp,
                            info->addr, info->rKey);
    rdmaSend.Execute();

    strcpy(serverBuff, "HellO worlD RDMA!");

    WaitForCompletion();

    check_z(ibv_dereg_mr(memReg));
    HandleDisconnect();
  }
};

class ServerCReads : Server {
public:

  ServerCReads() {
  }

  ~ServerCReads() {
  }

  void start(uint32_t entries) override {
    assert(eventChannel != NULL);
    assert(serverId != NULL);

    HandleConnectRequest();

    HandleConnectionEstablished();

    SendTDRdma sendRdma(protDomain, clientId->qp, entries);
    sendRdma.Execute();

    WaitForCompletion();
    HandleDisconnect();
  }
};

class ServCReadsFiltered : Server {
public:

  ServCReadsFiltered() {
  }

  ~ServCReadsFiltered() {
  }

  void start(uint32_t entries) override {
    assert(eventChannel != NULL);
    assert(serverId != NULL);

    D(std::cout << "RDMA filtered server (filtering occurs at server)\n");

    HandleConnectRequest();

    HandleConnectionEstablished();

    SendTDRdmaFiltered sendRdma(protDomain, clientId->qp, entries);
    sendRdma.filter(1);
    sendRdma.Execute();

    WaitForCompletion();
    HandleDisconnect();
  }
};

int main(int argc, char *argv[]) {
  opts opt = parse_cl(argc, argv);

  if (opt.read) {
    if (opt.filtered) {
      ServCReadsFiltered server;
      server.start(opt.entries);
    } else {
      ServerCReads server;
      server.start(opt.entries);
    }
  } else if (opt.write) {
    ServerSWrites server;
    server.start(opt.entries);
  } else if (opt.filtered) { // filtered server
    FServer server;
    server.start(opt.entries);
  } else { // unfiltered server
    Server server;
    server.start(opt.entries);
  }

  return 0;
}
