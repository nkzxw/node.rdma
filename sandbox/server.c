#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>       // usleep

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include <assert.h>

#include <uv.h>
#include <pthread.h>

#define ENABLE_DEBUG (1)

#ifdef ENABLE_DEBUG
#define DEBUGF(x) x
#else
#define DEBUGF(x) do { } while (0)
#endif

static uintptr_t thread1_id = 0;
static int prepare_cb_called = 0;
static int async1_cb_called = 0;

static uv_prepare_t prepare_handle;
static uv_async_t   async1_handle;

struct rdma_event_channel*  cm_channel;
struct rdma_cm_event*       event;

static int close_cb_called;
static int exit_cb_called;
static int on_read_cb_called;
static int after_write_cb_called;

static char exepath[1024];
static size_t exepath_size = 1024;
static char* args[3];

static uv_process_options_t options;
static uv_loop_t* loop;
uv_pipe_t out, in;

#define OUTPUT_SIZE 1024
static char output[OUTPUT_SIZE];
static int output_used;

#define COUNTOF(a) (sizeof(a) / sizeof(a[0]))

typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} write_req_t;

#ifndef WIN32
typedef void* (*uv_thread_cb)(void* arg);

uintptr_t uv_create_thread(void (*entry)(void* arg), void* arg) {
  pthread_t t;
  uv_thread_cb cb = (uv_thread_cb)entry;
  int r = pthread_create(&t, NULL, cb, arg);

  if (r) {
    return 0;
  }

  return (uintptr_t)t;
}

int uv_wait_thread(uintptr_t thread_id) {
  return pthread_join((pthread_t)thread_id, NULL);
}

void uv_sleep(int msec) {
  usleep(msec * 1000);
}

#endif

//
// --------------------------------------------------------------------------
//
#define RECV_WRID   (1)
#define SEND_WRID   (1)

int
post_recv(
  struct ibv_qp *qp,
  struct ibv_mr *mr,
  void *addr, uint32_t len)
{

  struct ibv_sge sge;
  sge.addr    = (uintptr_t)addr;
  sge.length  = len;
  sge.lkey    = mr->lkey;

  struct ibv_recv_wr wr;
  wr.wr_id   = RECV_WRID;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  struct ibv_recv_wr *bad_wr;

  int ret = ibv_post_recv(qp, &wr, &bad_wr);
  assert(ret == 0);

  return 0;
  
}

int
post_send(
  struct ibv_qp *qp,
  struct ibv_mr *mr,
  void *addr, uint32_t len)
{
  struct ibv_sge sge;
  sge.addr    = (uintptr_t)addr;
  sge.length  = len;
  sge.lkey    = mr->lkey;

  struct ibv_send_wr wr;
  wr.wr_id      = SEND_WRID;
  wr.opcode     = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.sg_list    = &sge;
  wr.num_sge    = 1;

  struct ibv_send_wr *bad_wr;

  int ret = ibv_post_send(qp, &wr, &bad_wr);
  assert(ret == 0);

  return 0;
}

static int
get_event(
  struct ibv_comp_channel *comp_chann)
{
  int ret = 0;
  int n = 0;

  struct ibv_cq *cq;
  struct ibv_wc wc;
 
  void *cq_context; // dummy

  printf("ibv_get_cq_event\n");
  ret = ibv_get_cq_event(comp_chann, &cq, &cq_context);
  assert(ret == 0);

  // NOTE: ibv_ack_cq_events takes a lock.
  // If you want a performance, less calling ibv_ack_cq_events.
  printf("ibv_ack_cq_events\n");
  ibv_ack_cq_events(cq, 1);

  // Request notification upon the next completion event. 
  printf("ibv_req_notify_cq\n");
  ret = ibv_req_notify_cq(cq, 0);
  assert(ret == 0);

  // Polls the cq for workd completions.
  n = ibv_poll_cq(cq, 1, &wc);
  assert(n == 1);

  // DO on_completion with wc.


  
  
}


//
// --------------------------------------------------------------------------
//

static void close_cb(uv_handle_t* handle) {
  printf("close_cb\n");
  close_cb_called++;
}

static void exit_cb(uv_process_t* process, int exit_status, int term_signal) {
  printf("exit_cb\n");
  exit_cb_called++;
  assert(exit_status == 0);
  assert(term_signal == 0);
  uv_close((uv_handle_t*)process, close_cb);
  uv_close((uv_handle_t*)&in, close_cb);
  uv_close((uv_handle_t*)&out, close_cb);
}

static uv_buf_t on_alloc(uv_handle_t* handle, size_t suggested_size) {
  uv_buf_t buf;
  buf.base = output + output_used;
  buf.len = OUTPUT_SIZE - output_used;
  return buf;
}

static uv_buf_t on_cm_event_alloc(uv_handle_t* handle, size_t suggested_size) {
   
    printf("--> on_cm_event_alloc\n"); fflush(stdout);

    uv_buf_t buf;
    buf.base = output + output_used;
    buf.len = 0;

    return buf;
}


static void after_write(uv_write_t* req, int status) {
  write_req_t* wr;

  if (status) {
    uv_err_t err = uv_last_error(loop);
    fprintf(stderr, "uv_write error: %s\n", uv_strerror(err));
    assert(0);
  }

  wr = (write_req_t*) req;

  /* Free the read/write buffer and the request */
  free(wr);

  after_write_cb_called++;
}



static void init_process_options(char* test, uv_exit_cb exit_cb)
{
    int r = uv_exepath(exepath, &exepath_size);
    assert(r == 0);
    exepath[exepath_size] = '\0';

    args[0] = exepath;
    args[1] = test;
    args[2] = NULL;

    options.file    = exepath;
    options.args    = args;
    options.exit_cb = exit_cb;
}

static void on_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t buf) {
  write_req_t* write_req;
  int r;
  uv_err_t err = uv_last_error(uv_default_loop());

  assert(nread > 0 || err.code == UV_EOF);

  if (nread > 0) {
    output_used += nread;
    if (output_used == 12) {
      assert(memcmp("hello world\n", output, 12) == 0);
      write_req = (write_req_t*)malloc(sizeof(*write_req));
      write_req->buf = uv_buf_init(output, output_used);
      r = uv_write(&write_req->req, (uv_stream_t*)&in, &write_req->buf, 1, after_write);
      assert(r == 0);
    }
  }

  on_read_cb_called++;
}

static void on_cm_event_read(uv_pipe_t* tcp, ssize_t nread, uv_buf_t buf, uv_handle_type pending) {
  printf("--> on_cm_evnet_read: nread = %ld, ptr = %p\n", nread, tcp);
  write_req_t* write_req;
  int r;
  uv_err_t err = uv_last_error(uv_default_loop());
  printf("err.code = %d\n", err.code);

  assert(nread > 0 || err.code == UV_EOF);

  if (nread > 0) {
    output_used += nread;
    if (output_used == 12) {
      assert(memcmp("hello world\n", output, 12) == 0);
      write_req = (write_req_t*)malloc(sizeof(*write_req));
      write_req->buf = uv_buf_init(output, output_used);
      r = uv_write(&write_req->req, (uv_stream_t*)&in, &write_req->buf, 1, after_write);
      assert(r == 0);
    }
  }

  on_read_cb_called++;
}

static uv_pipe_t stdin_pipe;
static uv_pipe_t stdout_pipe;
static int on_pipe_read_called;
static int after_write_called;

static void after_pipe_write(uv_write_t* req, int status) {
  assert(status == 0);
  after_write_called++;
}

void on_pipe_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t buf) {
  assert(nread > 0);
  assert(memcmp("hello world\n", buf.base, nread) == 0);
  on_pipe_read_called++;

  free(buf.base);

  uv_close((uv_handle_t*)&stdin_pipe, close_cb);
  uv_close((uv_handle_t*)&stdout_pipe, close_cb);
}


static uv_buf_t on_pipe_read_alloc(uv_handle_t* handle,
    size_t suggested_size) {
  uv_buf_t buf;
  buf.base = (char*)malloc(suggested_size);
  buf.len = suggested_size;
  return buf;
}

static struct ibv_pd* pd_;
static struct ibv_comp_channel* comp_channel_;
static struct ibv_cq* cq_;

static int on_connect_request(
  struct rdma_cm_id *id)

{
  struct rdma_conn_param conn_param;

  DEBUGF(printf("on_connect_request: id = %p\n", id));

  pd_ = ibv_alloc_pd(id->verbs);
  assert(pd_);

  comp_channel_ = ibv_create_comp_channel(id->verbs);
  assert(comp_channel_);

  cq_ = ibv_create_cq(id->verbs, 2, NULL, comp_channel_, 0);
  assert(cq_);

  int ret = ibv_req_notify_cq(cq_, 0);
  assert(ret == 0);

  struct ibv_qp_init_attr qp_attr = {};

  qp_attr.cap.max_send_wr   = 1;
  qp_attr.cap.max_send_sge  = 1;
  qp_attr.cap.max_recv_wr   = 1;
  qp_attr.cap.max_recv_sge  = 1;
  qp_attr.send_cq           = cq_;
  qp_attr.recv_cq           = cq_;
  qp_attr.qp_type           = IBV_QPT_RC;   // RC 

  printf("rmda_create_qp\n");
  ret = rdma_create_qp(id, pd_, &qp_attr);
  assert(ret == 0);

  printf("rmda_accept...\n");
  ret = rdma_accept(id, &conn_param);
  assert(ret);

  return 0;
}

static int on_established(
  struct rdma_cm_id *id)
{
  DEBUGF(printf("on_established: id = %p\n", id));
  return 0;
}

static int on_disconnect(
  struct rdma_cm_id *id)
{
  DEBUGF(printf("on_disconnect: id = %p\n", id));
  return 0;
}

void thread1_entry(void *arg)
{
  printf("thread entry...\n");
  printf("get cm event\n");
  int err = rdma_get_cm_event(cm_channel, &event);
  assert(!err);
  printf("get cm event ok\n");

  int r = 0;

  switch (event->event) {
    case RDMA_CM_EVENT_CONNECT_REQUEST:
      DEBUGF(printf("[DBG] CM: CONNECT_REQUEST\n"));
      r = on_connect_request(event->id);
      break;
    case RDMA_CM_EVENT_ESTABLISHED:
      DEBUGF(printf("[DBG] CM: ESTABLISHED\n"));
      r = on_established(event->id);
      break;
    case RDMA_CM_EVENT_DISCONNECTED:
      DEBUGF(printf("[DBG] CM: DISCONNECTED\n"));
      r = on_disconnect(event->id);
      break;
    default:
      fprintf(stderr, "Unsupported event. event = %d, id = %p\n", event->event, event->id);
      assert(0);
  }

  rdma_ack_cm_event(event);

  DEBUGF(printf("--> async_send.\n"));
  uv_async_send(&async1_handle);
  DEBUGF(printf("--> exit thread.\n"));
}

static void prepare_cb(
  uv_prepare_t* handle,
  int status)
{
  assert(handle == &prepare_handle);
  assert(status == 0);

  DEBUGF(printf("prepare_cb. # of called =  %d\n", prepare_cb_called)); 

  if (prepare_cb_called == 0) {

    thread1_id = uv_create_thread(thread1_entry, NULL);
    assert(thread1_id != 0);

  } else {

    uv_close((uv_handle_t*)handle, NULL);

  }

  prepare_cb_called++;

}

static void async1_cb(
  uv_async_t* handle,
  int status)
{
  static async1_closed = 0;

  DEBUGF(printf("async1_cb. called = %d\n", async1_cb_called));

  assert(handle == &async1_handle);
  assert(status == 0);

  if (!async1_closed) {
    async1_closed = 1;
    uv_close((uv_handle_t*)handle, NULL);
  }

  async1_cb_called++;
}

int
main(
    int argc,
    char **argv)
{
  int r;

  struct rdma_cm_id*          listen_id;
  struct rdma_cm_id*          cm_id;
  struct rdma_conn_param      conn_param = {};

  struct ibv_pd*              pd;
  struct ibv_comp_channel*    comp_channel;
  struct ibv_cq*              cq;
  struct ibv_cq*              evt_cq;
  struct ibv_mr*              mr;
  struct ibv_qp_init_attr     qp_attr = {};
  struct ibv_sge              sge;
  struct ibv_send_wr          send_wr = {};
  struct ibv_send_wr*         bad_send_wr;
  struct ibv_recv_wr          recv_wr = {};
  struct ibv_recv_wr*         bad_recv_wr;
  struct ibv_wc               wc;
  void*                       cq_context;

  struct sockaddr_in          sock_in;
  uint32_t*                   buf;
  int err;



  DEBUGF(printf("create_event_channel\n"));
  cm_channel = rdma_create_event_channel();
  assert(cm_channel);
  DEBUGF(printf("create_event_channel ok\n"));

  printf("create_id\n");
  err = rdma_create_id(cm_channel, &listen_id, NULL, RDMA_PS_TCP);  // IB RC
  assert(!err);
  printf("create_id ok\n");

  sock_in.sin_family      = AF_INET;
  sock_in.sin_port        = htons(20079);
  sock_in.sin_addr.s_addr = INADDR_ANY;

  err = rdma_bind_addr(listen_id, (struct sockaddr*)&sock_in);
  assert(!err);

  printf("listen\n");
  err = rdma_listen(listen_id, 1);
  assert(!err);
  printf("listen ok\n");

  loop = uv_default_loop();

  r = uv_prepare_init(loop, &prepare_handle);
  assert(r == 0);

  r = uv_prepare_start(&prepare_handle, prepare_cb);
  assert(r == 0);
    
  r = uv_async_init(loop, &async1_handle, async1_cb);

  printf("Running uv_run...\n");
  r = uv_run(loop);
  assert(r == 0);

  r = uv_wait_thread(thread1_id);
  assert(r == 0);
  printf("Running uv_run done.\n");

  printf("OK\n");
  exit(0);

  //assert(on_read_cb_called > 1);
  //assert(after_write_cb_called == 1);
  //assert(exit_cb_called == 1);
  //assert(close_cb_called == 3);
  //assert(memcmp("hello world\n", output, 12) == 0);
  //assert(output_used == 12);

  return 0;
}

/* vim: set ts=2 sw=2: */
