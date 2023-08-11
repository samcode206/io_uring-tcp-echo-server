#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
// #include <sys/mman.h>

#define FD_COUNT 64
#define SQ_DEPTH 1024
#define BUFFER_SIZE 4096
#define BUF_RINGS 1024 // must be power of 2
#define BG_ENTRIES 512 // must be power of 2
#define CONN_BACKLOG 256

#define FD_MASK ((1ULL << 21) - 1) // 21 bits
#define FD_SHIFT 0
#define SRV_LIM_MAX_FD 2097151
#define BGID_MASK (((1ULL << 17) - 1) << 21) // 17 bits shifted by 21
#define BGID_SHIFT 21
#define SRV_LIM_MAX_BGS 131071
#define EVENT_MASK (3ULL << 38) // 2 bits shifted by 38
#define EVENT_SHIFT 38

#define EV_ACCEPT 0
#define EV_RECV 1
#define EV_SEND 2
#define EV_CLOSE 3

#define FD_UNUSED -1
#define FD_CLOSING -2
#define FD_OPEN 1

static inline void set_fd(uint64_t *data, uint32_t fd) {
  *data = (*data & ~FD_MASK) | ((uint64_t)fd << FD_SHIFT);
}

static inline void set_bgid(uint64_t *data, uint32_t index) {
  *data = (*data & ~BGID_MASK) | ((uint64_t)index << BGID_SHIFT);
}

static inline void set_event(uint64_t *data, uint8_t event) {
  *data = (*data & ~EVENT_MASK) | ((uint64_t)event << EVENT_SHIFT);
}

static inline uint32_t get_fd(uint64_t data) {
  return (data & FD_MASK) >> FD_SHIFT;
}
static inline uint32_t get_bgid(uint64_t data) {
  return (data & BGID_MASK) >> BGID_SHIFT;
}
static inline uint8_t get_event(uint64_t data) {
  return (data & EVENT_MASK) >> EVENT_SHIFT;
}

typedef struct {
  struct io_uring ring;
  struct io_uring_buf_ring *buf_rings[BUF_RINGS];
  int fds[FD_COUNT];
  int active_bgid;
  char mapped_buffs[BUF_RINGS][BG_ENTRIES][BUFFER_SIZE];
} server_t;

server_t *server_init(void);

void server_register_buf_rings(server_t *s);
int server_socket_bind_listen(server_t *s, int port);
void server_ev_loop_start(server_t *s, int fd);

server_t *server_init(void) {
  // server_t *s = mmap(NULL, sizeof(server_t), PROT_READ | PROT_WRITE,
  //                    MAP_ANONYMOUS | MAP_SHARED | MAP_HUGETLB , -1, 0);
  // if (s == MAP_FAILED) {
  //   perror("mmap");
  //   exit(1);
  // }

  server_t *s = (server_t *)calloc(1, sizeof(server_t));
  assert(s != NULL);

  struct io_uring_params params;
  assert(memset(&params, 0, sizeof(params)) != NULL);

  assert(memset(s->fds, FD_UNUSED, sizeof(int) * FD_COUNT) != NULL);

  // params.cq_entries = CQ_ENTRIES; also add IORING_SETUP_CQSIZE to flags
  params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER |
                 IORING_SETUP_SUBMIT_ALL | IORING_SETUP_TASKRUN_FLAG |
                 IORING_SETUP_DEFER_TASKRUN;

  assert(io_uring_queue_init_params(SQ_DEPTH, &s->ring, &params) == 0);
  assert(io_uring_register_files_sparse(&s->ring, FD_COUNT) == 0);
  assert(io_uring_register_ring_fd(&s->ring) == 1);

  server_register_buf_rings(s);

  // unsigned int args[2] =  {0, 32};
  // int ret = io_uring_register_iowq_max_workers(&s->ring, args);
  // assert(ret == 0);
  // memset(args, 0, sizeof(args));
  // // call it again to get the current values after updating
  // io_uring_register_iowq_max_workers(&s->ring, args);
  // printf("server initialzed ring iow %d bounded: %d unbounded: %d\n", ret,
  // args[0], args[1]);

  return s;
}

void server_register_buf_rings(server_t *s) {
  unsigned short i;
  for (i = 0; i < BUF_RINGS; ++i) {
    int ret;
    struct io_uring_buf_ring *br =
        io_uring_setup_buf_ring(&s->ring, BG_ENTRIES, i, 0, &ret);
    assert(ret == 0);
    assert(br != NULL);
    io_uring_buf_ring_init(br);

    for (size_t j = 0; j < BG_ENTRIES; ++j) {
      io_uring_buf_ring_add(br, s->mapped_buffs[i][j], BUFFER_SIZE, j,
                            io_uring_buf_ring_mask(BG_ENTRIES), j);
    }

    io_uring_buf_ring_advance(br, BG_ENTRIES);
    s->buf_rings[i] = br;
  }
}

int server_socket_bind_listen(server_t *s, int port) {
  int fd;
  struct sockaddr_in srv_addr;

  fd = socket(PF_INET, SOCK_STREAM, 0);

  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(port);
  srv_addr.sin_addr.s_addr = htons(INADDR_ANY); /* 0.0.0.0 */

  assert(bind(fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr)) >= 0);
  assert(listen(fd, CONN_BACKLOG) >= 0);
  return fd;
}

struct io_uring_sqe *must_get_sqe(server_t *s) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&s->ring);
  if (!sqe) {
    io_uring_submit(&s->ring);
    sqe = io_uring_get_sqe(&s->ring);
    if (!sqe) {
      printf("failed to get an sqe shutting it down...\n");
      exit(1);
      return NULL;
    }
  }

  return sqe;
}

inline static char *server_get_selected_buffer(server_t *s, uint32_t bgid,
                                               uint32_t buf_idx) {
  return s->mapped_buffs[bgid][buf_idx];
}

inline static int server_get_active_bgid(server_t *s) { return s->active_bgid; }

inline static void server_active_bgid_next(server_t *s) {
  s->active_bgid = (s->active_bgid + 1) & (BUF_RINGS - 1);
}

inline static void server_release_one_buf(server_t *s, char *buf, uint32_t bgid,
                                          uint32_t buf_idx) {

  io_uring_buf_ring_add(s->buf_rings[bgid], buf, BUFFER_SIZE, buf_idx,
                        io_uring_buf_ring_mask(BG_ENTRIES), 0);

  io_uring_buf_ring_advance(s->buf_rings[bgid], 1);
}

int server_add_multishot_accept(server_t *s, int listener_fd) {
  struct io_uring_sqe *accept_ms_sqe = must_get_sqe(s);
  struct sockaddr_in client_addr;

  socklen_t client_addr_len = sizeof(client_addr);
  assert(accept_ms_sqe != NULL);
  io_uring_prep_multishot_accept_direct(accept_ms_sqe, listener_fd,
                                        (struct sockaddr *)&client_addr,
                                        &client_addr_len, 0);

  uint64_t accept_ctx = 0;
  set_event(&accept_ctx, EV_ACCEPT);
  io_uring_sqe_set_data64(accept_ms_sqe, accept_ctx);

  return 0;
}

int server_add_multishot_recv(server_t *s, int fd) {
  struct io_uring_sqe *sqe = must_get_sqe(s);
  io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
  io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT);
  uint64_t recv_ctx = 0;
  set_event(&recv_ctx, EV_RECV);
  set_fd(&recv_ctx, fd);
  set_bgid(&recv_ctx, server_get_active_bgid(s));
  io_uring_sqe_set_data64(sqe, recv_ctx);
  sqe->buf_group = server_get_active_bgid(s);
  return 0;
}

void server_add_close_direct(server_t *s, uint32_t fd) {
  if (s->fds[fd] != FD_CLOSING) {
    struct io_uring_sqe *sqe = must_get_sqe(s);

    sqe->fd = fd;
    s->fds[fd] = FD_CLOSING;
    io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
    io_uring_prep_close_direct(sqe, fd);

    uint64_t close_ctx = 0;
    set_event(&close_ctx, EV_CLOSE);
    set_fd(&close_ctx, fd);

    io_uring_sqe_set_data64(sqe, close_ctx);
  }
}

static inline void server_handle_recv_err(server_t *s, int err, uint64_t ctx) {
  if (err == 0) {
    server_add_close_direct(s, get_fd(ctx));
  } else if (err == -ENOBUFS) {
    // printf("ran out of buffers for gid: %d ", get_bgid(ctx));
    server_active_bgid_next(s);
    // printf("moving to next buffer group id: %d\n",
    // server_get_active_bgid(s));
    server_add_multishot_recv(s, get_fd(ctx));
  } else {
    printf("RECV err: %d\n", err);
    server_add_close_direct(s, get_fd(ctx));
  }
}

static inline void server_add_send(server_t *s, uint32_t fd, const void *data,
                                   size_t len) {
  struct io_uring_sqe *sqe = must_get_sqe(s);
  io_uring_prep_send(sqe, fd, data, len, 0);
  io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
  uint64_t send_ctx = 0;
  set_fd(&send_ctx, fd);
  set_event(&send_ctx, EV_SEND);
  io_uring_sqe_set_data64(sqe, send_ctx);
}

static inline void server_add_send_skip_success(server_t *s, uint32_t fd,
                                                const void *data, size_t len) {
  struct io_uring_sqe *sqe = must_get_sqe(s);
  io_uring_prep_send(sqe, fd, data, len, 0);
  io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE | IOSQE_CQE_SKIP_SUCCESS);
  uint64_t send_ctx = 0;
  set_fd(&send_ctx, fd);
  set_event(&send_ctx, EV_SEND);
  io_uring_sqe_set_data64(sqe, send_ctx);
}

void server_ev_loop_start(server_t *s, int listener_fd) {
  server_add_multishot_accept(s, listener_fd);

  for (;;) {
    // printf("start loop iteration\n");
    int ret = io_uring_submit_and_wait(&s->ring, 1);
    assert(ret >= 0); // todo remove and add real error handling

    // printf("io_uring_submit_and_wait: %d\n", ret);
    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned i = 0;

    io_uring_for_each_cqe(&s->ring, head, cqe) {
      ++i;
      uint64_t ctx = io_uring_cqe_get_data64(cqe);
      uint8_t ev = get_event(ctx);

      switch (ev) {
      case EV_RECV:
        if (cqe->res <= 0) {
          server_handle_recv_err(s, cqe->res, ctx);
        } else {
          // printf("RECV %d\n", cqe->res);
          unsigned int buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
          uint32_t bgid = get_bgid(ctx);
          // printf("buffer-group: %d\tbuffer-id: %d\n", bgid, buf_id);
          char *recv_buf = server_get_selected_buffer(s, bgid, buf_id);
          // printf("%s\n", recv_buf);
          uint32_t sender_fd = get_fd(ctx);
          for (size_t i = 0; i < FD_COUNT; ++i) {
            if (s->fds[i] == FD_OPEN && i != sender_fd) {
              server_add_send_skip_success(s, i, recv_buf, cqe->res);
            }
          }

          server_release_one_buf(s, recv_buf, bgid, buf_id);
        }
        break;
      case EV_ACCEPT:
        if (cqe->res < 0) {
          printf("accept error: %d exiting...\n", cqe->res);
          exit(1);
        }
        printf("ACCEPT %d\n", cqe->res);
        s->fds[cqe->res] = FD_OPEN;
        server_add_multishot_recv(s, cqe->res);
        break;
      case EV_SEND:
        if (cqe->res <= 0) {
          printf("SEND FAILURE %d\n", cqe->res);
        } else {
          printf("SEND %d\n", cqe->res);
        }
        break;
      case EV_CLOSE:
        if (cqe->res == 0) {
          uint32_t closed_fd = get_fd(ctx);
          printf("file closed %d\n", closed_fd);
          s->fds[closed_fd] = FD_UNUSED;
        } else {
          printf("close error: %d\n", cqe->res);
        }
        break;
      }
    };
    // printf("end loop iteration cqes seen %d\n", i);
    io_uring_cq_advance(&s->ring, i);
  }
}

int main(void) {
  server_t *s = server_init();
  assert(s != NULL);
  int fd = server_socket_bind_listen(s, 9919);
  printf("server starting on port: %d\n", 9919);
  server_ev_loop_start(s, fd);

  close(fd);

  return 0;
}
