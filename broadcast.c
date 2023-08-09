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
#define EV_RECV 1
#define EV_SEND 2
#define EV_CLOSE 3

#define CONN_BACKLOG 512
#define MAX_CONNS 1024
#define MAX_REQUESTS 1000000
#define IO_URING_MAX_ENTRIES 4096

#define MAX_BUFF_SIZE 4096

#define IS_EOF(ret) ret == 0
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

void log_fatal(const char* reason) {
  printf("%s\n", reason);
  exit(1);
}

struct conn {
  int fd;
  char* data;
};

struct request {
  size_t id;
  int ev_type;
  struct conn* conn;
};

struct broadcast {
  struct io_uring ring;

  struct conn conns[MAX_CONNS];
  size_t max_fd;

  struct request reqs[MAX_REQUESTS];
  size_t num_reqs;
};

struct broadcast* broadcast_init() {
  struct broadcast* b = (struct broadcast*)malloc(sizeof(struct broadcast));
  if (!b) return NULL;
  b->max_fd = 4;

  struct io_uring_params params;
  memset(&params, 0, sizeof(params));
  params.flags = IORING_SETUP_SINGLE_ISSUER;

  if (io_uring_queue_init_params(IO_URING_MAX_ENTRIES, &b->ring, &params) < 0) {
    log_fatal("io_uring_init_failed...");
  }

  for (size_t i = 0; i < MAX_CONNS; ++i) {
    b->conns[i].data = NULL;
    b->conns[i].fd = -1;
  }

  for (size_t i = 0; i < MAX_REQUESTS; ++i) {
    b->reqs[i].conn = NULL;
    b->reqs[i].id = i;
    b->reqs[i].ev_type = EV_UNDEFINED;
  };

  b->num_reqs = 0;
  return b;
}

struct conn* broadcast_conn_reserve(struct broadcast* b, int fd) {
  if (fd < MAX_CONNS) {
    b->conns[fd].fd = fd;

    if (b->max_fd < fd) {
      b->max_fd = fd;
    }

    return &b->conns[fd];
  }
  return NULL;
}

void broadcast_conn_put(struct broadcast* b, struct conn* c) {
  if (c->data) {
    free(c->data);
    c->data = NULL;
  };

  if (c->fd == b->max_fd) {
    b->max_fd = b->max_fd - 1;
  }
  c->fd = -1;
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
  }

  return NULL;
}

static inline void broadcast_request_put(struct broadcast* b,
                                         struct request* r) {
  b->reqs[r->id].ev_type = EV_UNDEFINED;
  b->reqs[r->id].conn = NULL;
  --b->num_reqs;
}

static inline void request_set_conn(struct request* r, struct conn* c) {
  r->conn = c;
}

char* conn_get_data(struct conn* c) { return c->data; }

char* conn_prep_data(struct conn* c) {
  char* data = conn_get_data(c);
  if (!data) {
    c->data = malloc(sizeof(char) * MAX_BUFF_SIZE);
    if (!c->data) {
      return NULL;
    }
    return c->data;
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

void ev_loop_add_multishot_accept(struct broadcast* b, int fd,
                                  struct sockaddr_in* client_addr,
                                  socklen_t* client_addr_len) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(&b->ring);

  io_uring_prep_multishot_accept_direct(sqe, fd, (struct sockaddr*)client_addr,
                                        client_addr_len, 0);

  io_uring_sqe_set_data(sqe, NULL);
}

int ev_loop_add_recv(struct broadcast* b, struct request* req) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(&b->ring);
  if (!sqe) {
    return -1;
  }

  char* data = conn_prep_data(req->conn);
  if (!data) {
    broadcast_request_put(b, req);
    return -1;
  }

  int fd = conn_get_fd(req->conn);
  io_uring_prep_recv(sqe, fd, data, MAX_BUFF_SIZE, 0);
  sqe->flags |= IOSQE_FIXED_FILE;
  sqe->fd = fd;
  io_uring_sqe_set_data(sqe, req);

  return 0;
}

int ev_loop_add_close(struct broadcast* b, struct request* req) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(&b->ring);
  if (!sqe) {
    return -1;
  }

  sqe->fd = req->conn->fd;
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_prep_close(sqe, req->conn->fd);

  req->ev_type = EV_CLOSE;
  io_uring_sqe_set_data(sqe, req);
  return 0;
}

int ev_loop_add_send(struct broadcast* b, struct conn* conn_receiver,
                     const void* data, size_t len) {
  struct request* req = broadcast_request_reserve(b, EV_SEND);
  if (!req) {
    printf("no more reqs\n");
    return -1;
  }
  request_set_conn(req, conn_receiver);
  struct io_uring_sqe* sqe = io_uring_get_sqe(&b->ring);
  if (!sqe) {
    broadcast_request_put(b, req);
    return -1;
  }

  sqe->fd = conn_receiver->fd;
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_prep_send(sqe, conn_receiver->fd, data, len, 0);
  io_uring_sqe_set_data(sqe, req);
  return 0;
}

int ev_loop_init(int server_fd, struct broadcast* b) {
  struct io_uring_cqe* cqe;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  ev_loop_add_multishot_accept(b, server_fd, &client_addr, &client_addr_len);
  unsigned int pending_sqe = 1;

  for (;;) {
    if (pending_sqe) {
      int ret = io_uring_submit_and_wait(&b->ring, 1);
      if (ret < 0) {
        perror("io_uring_wait_cqe");
        break;
      }
      pending_sqe = 0;
    }

    for (;;) {
      int ret = io_uring_peek_cqe(&b->ring, &cqe);
      if (ret == -EAGAIN) {
        break;
      }
      struct request* req = io_uring_cqe_get_data(cqe);
      if (req == NULL) {
        printf("ACCEPT fd:%d \n", cqe->res);
        struct conn* new_conn = broadcast_conn_reserve(b, cqe->res);
        if (!new_conn) {
          // handle
          log_fatal("broadcast_conn_reserve");
        }

        struct request* r = broadcast_request_reserve(b, EV_RECV);
        if (!r) {
          // handle
          log_fatal("broadcast_request_reserve");
        }
        request_set_conn(r, new_conn);

        if (ev_loop_add_recv(b, r) == -1) {
          log_fatal("ev_loop_add_recv");
        };
        ++pending_sqe;
      } else {
        switch (req->ev_type) {
          case EV_RECV:
            printf("RECV\n");
            if (UNLIKELY(cqe->res <= 0)) {
              if (IS_EOF(cqe->res)) {
                printf("client disconnected\n");
              } else {
                perror("recv");
              }
              if (req->conn->fd != -1) {
                if (ev_loop_add_close(b, req) == -1) {
                  log_fatal("ev_loop_add_close");
                };
                ++pending_sqe;
              }
            } else {
              ev_loop_add_recv(b, req);
              ++pending_sqe;

              // printf("%s\n", conn_get_data(req->conn));
              // start from first client fd until max seen fd +1
              for (size_t i = 0; i < b->max_fd + 1; ++i) {
                if ((b->conns[i].fd != -1) &&
                    (b->conns[i].fd != req->conn->fd)) {
                  if (ev_loop_add_send(b, &b->conns[i],
                                       conn_get_data(req->conn),
                                       cqe->res) == -1) {
                    printf("io_uring_sq_space_left: %d\n",
                           io_uring_sq_space_left(&b->ring));
                    log_fatal("ev_loop_add_send");
                  };
                  ++pending_sqe;
                }
              }
            }
            break;
          case EV_SEND:
            if (cqe->res < 0) {
              if (req->conn->fd != -1) {
                req->ev_type = EV_CLOSE;
                ev_loop_add_close(b, req);
                req->conn->fd = -1;
                ++pending_sqe;
              } else {
                broadcast_request_put(b, req);
              }
            } else {
              // printf("SEND\n");
              broadcast_request_put(b, req);
            }
            break;
          case EV_CLOSE:
            printf("CLOSE\n");
            printf("close results: %d\n", cqe->res);
            struct conn* closed_conn = req->conn;
            broadcast_request_put(b, req);
            broadcast_conn_put(b, closed_conn);
            break;
        }
      }

      io_uring_cqe_seen(&b->ring, cqe);
    }
  }

  return 0;
}

int main() {
  int server_fd = must_listener_socket_init(PORT);

  struct broadcast* b = broadcast_init();

  assert(io_uring_register_files_sparse(&b->ring, MAX_CONNS) == 0);
  assert(io_uring_register_ring_fd(&b->ring) > 0);

  // TODO(sah): add signal handlers before starting event loop
  printf("Starting Sever on port: %d\n", PORT);
  ev_loop_init(server_fd, b);
}
