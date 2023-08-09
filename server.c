#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FDS 1024
#define SQ_ENTRIES 1024

#define BUFFER_SIZE 1024 * 4
#define BUF_RINGS 1
#define BG_ENTRIES 1024 * 4
#define CONN_BACKLOG 256

typedef struct {
  struct io_uring ring;
  struct io_uring_buf_ring *buf_rings[BUF_RINGS];
  int fds[MAX_FDS];
} server_t;

server_t *server_init(void);
struct io_uring_buf_ring *server_register_bg(server_t *s, unsigned short bgid,
                                             unsigned int entries, char *buf,
                                             unsigned int buf_sz);
void server_register_buf_rings(server_t *s);
int server_socket_bind_listen(server_t *s, int port);
void server_ev_loop_start(server_t *s, int fd);

server_t *server_init(void) {
  server_t *s = (server_t *)calloc(1, sizeof(server_t));
  assert(s != NULL);

  struct io_uring_params params;
  assert(memset(&params, 0, sizeof(params)) != NULL);

  // params.cq_entries = CQ_ENTRIES; also add IORING_SETUP_CQSIZE to flags
  params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

  assert(io_uring_queue_init_params(SQ_ENTRIES, &s->ring, &params) == 0);
  assert(io_uring_register_files_sparse(&s->ring, MAX_FDS) == 0);
  assert(io_uring_register_ring_fd(&s->ring) == 1);

  server_register_buf_rings(s);

  return s;
}
void server_register_buf_rings(server_t *s) {
  unsigned int i;
  for (i = 0; i < BUF_RINGS; ++i) {
    char *group_buf = (char *)calloc(BG_ENTRIES * BUFFER_SIZE, sizeof(char));
    assert(group_buf != NULL);
    s->buf_rings[i] =
        server_register_bg(s, 0, BG_ENTRIES, group_buf, BUFFER_SIZE);
    assert(s->buf_rings[i] != NULL);
  }
}

struct io_uring_buf_ring *server_register_bg(server_t *s, unsigned short bgid,
                                             unsigned int entries, char *buf,
                                             unsigned int buf_sz) {
  struct io_uring_buf_ring *br;
  struct io_uring_buf_reg reg = {0};

  posix_memalign((void **)&br, getpagesize(),
                 entries * sizeof(struct io_uring_buf_ring));

  reg.ring_addr = (unsigned long)br;
  reg.ring_entries = entries;
  reg.bgid = bgid;

  assert(io_uring_register_buf_ring(&s->ring, &reg, 0) == 0);
  io_uring_buf_ring_init(br);

  unsigned int i;
  unsigned int offset = 0;
  for (i = 0; i < entries; ++i) {
    io_uring_buf_ring_add(br, buf + offset, buf_sz, i,
                          io_uring_buf_ring_mask(entries), i);
    offset += buf_sz;
  }

  io_uring_buf_ring_advance(br, entries);

  return br;
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

void server_ev_loop_start(server_t *s, int fd) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&s->ring);
  struct sockaddr_in client_addr;

  socklen_t client_addr_len = sizeof(client_addr);
  assert(sqe != NULL);
  io_uring_prep_multishot_accept_direct(
      sqe, fd, (struct sockaddr *)&client_addr, &client_addr_len, 0);
  io_uring_sqe_set_data(sqe, NULL);

  for (;;) {
    io_uring_submit_and_wait(&s->ring, 1);
    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned i = 0;

    io_uring_for_each_cqe(&s->ring, head, cqe) {
      ++i;
      void *ctx = io_uring_cqe_get_data(cqe);
      if (!ctx){
        // ACCEPT
        printf("ACCEPT %d\n", cqe->res);
        struct io_uring_sqe *sqe = io_uring_get_sqe(&s->ring);
        if (!sqe){
          io_uring_submit(&s->ring);
          sqe = io_uring_get_sqe(&s->ring);
          if (!sqe){
            printf("failed to get an sqe...\n");
            exit(1);
          }
        }
        
        io_uring_prep_recv_multishot(sqe, cqe->res, NULL, 0, 0);
        sqe->fd = cqe->res;
        sqe->flags |= IOSQE_FIXED_FILE;
        

      }

    };

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
