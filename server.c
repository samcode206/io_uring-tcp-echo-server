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

#define EV_ACCEPT 1
#define EV_RECV 2
#define EV_SEND 3
#define EV_CLOSE 4

#define IS_EOF(ret) ret == 0

struct request {
  int ev_type;
  int fd;
  char* data;
};

void log_fatal(const char* fn_name) {
  perror(fn_name);
  exit(1);
}

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

  if (listen(fd, 10) < 0) log_fatal("listen()");

  return fd;
}

int ev_loop_add_accept(struct io_uring* ring, int fd,
                       struct sockaddr_in* client_addr,
                       socklen_t* client_addr_len) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(ring);

  io_uring_prep_accept(sqe, fd, (struct sockaddr*)client_addr, client_addr_len,
                       0);

  struct request* req = malloc(sizeof(struct request));
  req->ev_type = EV_ACCEPT;
  req->fd = fd;
  req->data = NULL;
  io_uring_sqe_set_data(sqe, req);

  return 0;
}

int ev_loop_add_recv(struct io_uring* ring, int fd) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
  struct request* req = malloc(sizeof(struct request));
  req->ev_type = EV_RECV;
  req->fd = fd;
  req->data = malloc(sizeof(char) * 4096);
  io_uring_prep_recv(sqe, fd, req->data, 4096, 0);
  io_uring_sqe_set_data(sqe, req);

  return 0;
}

int ev_loop_add_send(struct io_uring* ring, int fd, struct request* req,
                     size_t len) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(ring);

  req->ev_type = EV_SEND;
  io_uring_prep_send(sqe, fd, req->data, len, 0);
  io_uring_sqe_set_data(sqe, req);
  return 0;
}

int ev_loop_init(int server_fd, struct io_uring* ring) {
  struct io_uring_cqe* cqe;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  ev_loop_add_accept(ring, server_fd, &client_addr, &client_addr_len);
  io_uring_submit(ring);

  size_t submissions = 0;

  for (;;) {
    if (io_uring_wait_cqe(ring, &cqe) < 0) log_fatal("io_uring_wait_cqe");
    struct request* req = (struct request*)cqe->user_data;

    switch (req->ev_type) {
      case EV_ACCEPT:
        ev_loop_add_accept(ring, server_fd, &client_addr, &client_addr_len);
        ev_loop_add_recv(ring, cqe->res);
        submissions += 2;
        free(req);
        printf("ACCEPT\n");
        break;
      case EV_RECV:
        if (cqe->res <= 0) {
          if (IS_EOF(cqe->res)) {
            printf("client disconnected\n");
          } else {
            perror("recv");
          }
          break;
        }

        printf("%s\n", req->data);
        printf("RECV\n");
        ev_loop_add_recv(ring, req->fd);
        ev_loop_add_send(ring, req->fd, req, cqe->res);
        submissions += 2;
        break;
      case EV_SEND:
        if (req->data) {
          free(req->data);
          free(req);
        }
        printf("SEND\n");
        break;
      case EV_CLOSE:
        printf("CLOSE\n");
        break;
    }

    io_uring_cqe_seen(ring, cqe);

    if (submissions) {
      io_uring_submit(ring);
      submissions = 0;
    }
  }

  return 0;
}

int main() {
  int server_fd = must_listener_socket_init(PORT);
  struct io_uring ring;
  assert(io_uring_queue_init(256, &ring, 0) > -1);

  // TODO(sah): add signal handlers before starting event loop
  printf("Starting Sever on port: %d\n", PORT);
  ev_loop_init(server_fd, &ring);
}
