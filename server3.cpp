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
  Server() : serverId(NULL), clientId(NULL), memReg(NULL) {

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
    send.exec();

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
    send.exec();

    WaitForCompletion();

    timer_end(t0);
    HandleDisconnect();
  }
};

class ServerSWrites : Server {
public:

  ServerSWrites() {
  }

  ~ServerSWrites() {
  }

  void start(uint32_t entries) override {
    assert(eventChannel != NULL);
    assert(serverId != NULL);

    HandleConnectRequest();

    TestData *Data = new TestData[entries]();
    initData(Data, entries, 3);

    auto t0 = timer_start();

    RecvSI ReceiveSI(protDomain);
    ReceiveSI.post(clientId->qp);

    HandleConnectionEstablished();
    WaitForCompletion();

    ReceiveSI.print();

    std::vector<TestData> Vec = filterData(ReceiveSI.Info->ReqKey, Data, entries);
    TestData *Filtered = vecToArray(Vec);

    // RDMA write
    size_t WriteSize = Vec.size() * sizeof(TestData);
    MemRegion WriteMR(Filtered, WriteSize, protDomain);
    Sge WriteSGE((uint64_t) Filtered, WriteSize, WriteMR.getRegion()->lkey);
    SendWR WriteWR(WriteSGE);
    WriteWR.setOpcode(IBV_WR_RDMA_WRITE_WITH_IMM);
    WriteWR.setRdma(ReceiveSI.Info->Addr, ReceiveSI.Info->RemoteKey);
    WriteWR.post(clientId->qp);

    // zero-byte send
    SendWR ZeroWR;
    ZeroWR.setOpcode(IBV_WR_SEND);
    ZeroWR.post(clientId->qp);

    WaitForCompletion();

    timer_end(t0);

    delete[] Filtered;
    delete[] Data;
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
    sendRdma.exec();

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

    SendSIFilter sendRdma(protDomain, clientId->qp, entries);
    sendRdma.filter(1);
    sendRdma.exec();

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
