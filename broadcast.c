#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT 9919

#define EV_UNDEFINED 0
#define EV_ACCEPT 1
#define EV_RECV 2
#define EV_SEND 3
#define EV_CLOSE 4


#define CONN_BACKLOG 512
#define MAX_CONNS 1024
#define MAX_REQUESTS 4096
#define IO_URING_MAX_ENTRIES 1024

#define IS_EOF(ret) ret == 0
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

void log_fatal(const char* fn_name) {
  perror(fn_name);
  exit(1);
}

struct conn {
  int fd;
  size_t b_idx;

  char* data;
  size_t offset;
};

struct request {
  size_t id;
  int ev_type;
  struct conn* conn;
};

struct broadcast {
  struct io_uring ring;

  struct conn conns[MAX_CONNS];
  size_t num_conns;

  struct request reqs[MAX_REQUESTS];
  size_t num_reqs;
};

struct broadcast* broadcast_init() {
  struct broadcast* b = (struct broadcast*)malloc(sizeof(struct broadcast));
  b->num_conns = 0;
  int ret = io_uring_queue_init(IO_URING_MAX_ENTRIES, &b->ring, 0);
  if (ret < 0) log_fatal("io_uring_queue_init");

  for (size_t i = 0; i < MAX_REQUESTS; ++i) {
    b->reqs[i].conn = NULL;
    b->reqs[i].id = i;
    b->reqs[i].ev_type = EV_UNDEFINED;
  };

  b->num_reqs = 0;
  return b;
}

struct conn* broadcast_conn_reserve(struct broadcast* b, int fd) {
  if (b->num_conns + 1 < MAX_CONNS) {
    struct conn* c = &b->conns[b->num_conns++];
    c->b_idx = b->num_conns;
    c->data = NULL;
    c->fd = fd;
    c->offset = 0;

  } else {
    return NULL;
  }
}

void broadcast_conn_put(struct broadcast* b, struct conn* c) {
  /* TODO(sah): implement */
}

struct request* broadcast_request_reserve(struct broadcast* b, int ev_type) {
  if (b->num_reqs + 1 < MAX_REQUESTS) {
    for (size_t i = 0; i < MAX_REQUESTS; ++i) {
      if (b->reqs[i].ev_type == EV_UNDEFINED) {
        ++b->num_reqs;
        b->reqs[i].ev_type = ev_type;
        return &b->reqs[i];
      }
    }
  } else {
    return NULL;
  }
}

static inline void broadcast_request_put(struct broadcast* b,
                                         struct request* r) {
  b->reqs[r->id].ev_type = EV_UNDEFINED;
  b->reqs[r->id].conn = NULL;
}

static inline void request_set_conn(struct request* r, struct conn* c) {
  r->conn = c;
}

char* conn_get_data(struct conn* c) { return c->data; }

size_t conn_get_data_offset(struct conn* c) { return c->offset; }

char* conn_prep_data(struct conn* c) {
  char* data = conn_get_data(c);
  if (!data) {
    c->data = malloc(sizeof(4096));
    if (!c->data) {
      return NULL;
    }
  } else {
    return data;
  }
}

int conn_get_fd(struct conn* c) { return c->fd; }

int must_listener_socket_init(int port) {
  int fd;
  struct sockaddr_in srv_addr;

  fd = socket(PF_INET, SOCK_STREAM, 0);
  if (fd < 0) log_fatal("socket");

  int on = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int)) < 0)
    log_fatal("setsockopt");

  assert(memset(&srv_addr, 0, sizeof(srv_addr)) != NULL);
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(port);
  srv_addr.sin_addr.s_addr = htons(INADDR_ANY); /* 0.0.0.0 */

  if (bind(fd, (const struct sockaddr*)&srv_addr, sizeof(srv_addr)) < 0)
    log_fatal("bind()");

  if (listen(fd, CONN_BACKLOG) < 0) log_fatal("listen()");

  return fd;
}

int ev_loop_add_accept(struct broadcast* b, int fd,
                       struct sockaddr_in* client_addr,
                       socklen_t* client_addr_len) {
  struct request* req = broadcast_request_reserve(b, EV_ACCEPT);
  if (!req) return -1;

  struct io_uring_sqe* sqe = io_uring_get_sqe(&b->ring);

  io_uring_prep_accept(sqe, fd, (struct sockaddr*)client_addr, client_addr_len,
                       0);

  io_uring_sqe_set_data(sqe, req);
  return 0;
}

int ev_loop_add_recv(struct broadcast* b, struct conn* c) {
  struct request* req = broadcast_request_reserve(b, EV_RECV);
  if (UNLIKELY(req == NULL)) {
    return -1;
  }
  request_set_conn(req, c);

  struct io_uring_sqe* sqe = io_uring_get_sqe(&b->ring);

  char* data = conn_prep_data(c);
  if (!data) {
    broadcast_request_put(b, req);
    return -1;
  }

  int fd = conn_get_fd(c);

  size_t offset = conn_get_data_offset(c);
  data += offset;

  io_uring_prep_recv(sqe, fd, data, 4096 - offset, 0);

  io_uring_sqe_set_data(sqe, req);

  return 0;
}

int ev_loop_add_send(struct broadcast* b, struct conn* conn_receiver,
                     const void* data, size_t len) {
  struct request* req = broadcast_request_reserve(b, EV_SEND);
  if (!req) {
    return -1;
  }
  request_set_conn(req, conn_receiver);
  struct io_uring_sqe* sqe = io_uring_get_sqe(&b->ring);
  io_uring_prep_send(sqe, conn_receiver->fd, data, len, 0);
  io_uring_sqe_set_data(sqe, req);
  return 0;
}

int ev_loop_init(int server_fd, struct broadcast* b) {
  struct io_uring_cqe* cqe;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  ev_loop_add_accept(b, server_fd, &client_addr, &client_addr_len);
  io_uring_submit(&b->ring);

  size_t submissions = 0;

  for (;;) {
    if (io_uring_wait_cqe(&b->ring, &cqe) < 0) log_fatal("io_uring_wait_cqe");
    struct request* req = (struct request*)io_uring_cqe_get_data(cqe);

    switch (req->ev_type) {
      case EV_ACCEPT:
        printf("ACCEPT\n");
        if (ev_loop_add_accept(b, server_fd, &client_addr, &client_addr_len) <
            0) {
          perror("ev_loop_add_accept");
          break;
        };

        if (cqe->res < 0) {
          perror("accept");
          break;
        }

        broadcast_request_put(b, req);
        struct conn* new_conn = broadcast_conn_reserve(b, cqe->res);
        ev_loop_add_recv(b, new_conn);
        submissions += 2;  // ACCEPT, RECV submitted
        break;
      case EV_RECV:
        printf("RECV\n");
        if (UNLIKELY(cqe->res <= 0)) {
          if (IS_EOF(cqe->res)) {
            printf("client disconnected\n");
          } else {
            perror("recv");
          }
          break;
        }

        struct conn* sender_conn = req->conn;
        broadcast_request_put(b, req);
        ev_loop_add_recv(b, sender_conn);

        ++submissions;

        for (size_t i = 0; i < b->num_conns; ++i) {
          if (b->conns[i].fd != req->conn->fd) {
            ev_loop_add_send(b, &b->conns[i], conn_get_data(sender_conn),
                             cqe->res);
          }
          ++submissions;
        }

        break;
      case EV_SEND:
        printf("SEND\n");
        broadcast_request_put(b, req);
        break;
      case EV_CLOSE:
        printf("CLOSE\n");
        break;
    }

    io_uring_cqe_seen(&b->ring, cqe);

    if (submissions) {
      io_uring_submit(&b->ring);
      submissions = 0;
    }
  }

  return 0;
}

int main() {
  int server_fd = must_listener_socket_init(PORT);

  struct broadcast* b = broadcast_init();

  // TODO(sah): add signal handlers before starting event loop
  printf("Starting Sever on port: %d\n", PORT);
  ev_loop_init(server_fd, b);
}
