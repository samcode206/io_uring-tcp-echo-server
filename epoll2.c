#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT "9919"
#define BACKLOG 100
#define BUF_SZ 1024 * 64

#define MAX_EVENTS 4096
#define MAX_CONNS 1024 + 4

#define IS_SERVER_FD_EVENT(e, sfd) ((e).data.fd == (sfd))

#define EREAD 1
#define EWRITE 2
#define ERW 3
#define ESHOULDCLOSE 4

typedef struct {
  int readable;
  int writeable;

  int fd;
  ssize_t off_buf;
  char buf[BUF_SZ];
} conn_t;

typedef struct {
  conn_t *conn;
  int events;
} evqe;

typedef struct {
  int server_fd;
  int ep_fd;
  struct epoll_event ep_evs[MAX_EVENTS];
  conn_t conns[MAX_CONNS];
  size_t num_cons;

  evqe ev_q[MAX_EVENTS];
  ssize_t ev_q_head;
  ssize_t ev_q_tail;

} server_t;

server_t *server_new(int fd) {
  server_t *s = calloc(1, sizeof(server_t));
  return s;
}

int server_epoll_init(server_t *s) {
  int fd = epoll_create1(EPOLL_CLOEXEC);
  if (fd < 0) {
    return fd;
  }
  s->ep_fd = fd;
  return 0;
}

static inline void server_set_fd(server_t *s, int fd) { s->server_fd = fd; }

static inline int server_get_fd(server_t *s) { return s->server_fd; }

static inline size_t server_evq_get_head(server_t *s) { return s->ev_q_head; }

static inline size_t server_evq_get_tail(server_t *s) { return s->ev_q_tail; }

static inline void server_evq_move_head(server_t *s, int d) {
  s->ev_q_head = (s->ev_q_head + d) & (MAX_EVENTS - 1);
}

static inline void server_evq_move_tail(server_t *s, int d) {
  s->ev_q_tail = (s->ev_q_tail + d) & (MAX_EVENTS - 1);
}

static inline evqe *server_evq_peek_evqe(server_t *s) {
  if (s->ev_q_head != s->ev_q_tail) {
    return &s->ev_q[s->ev_q_tail];
  }

  return NULL;
}

static evqe *server_evq_add_evqe(server_t *s, conn_t *c, int events) {
  if (((s->ev_q_head + 1) & (MAX_EVENTS - 1)) != s->ev_q_tail) {
    evqe *qe = &s->ev_q[s->ev_q_head];
    qe->events = events;
    qe->conn = c;
    server_evq_move_head(s, 1);
    return qe;
  }
  return NULL;
}

static int server_evq_delete_evqe(server_t *s) {
  if (((s->ev_q_tail + 1) & (MAX_EVENTS - 1)) <= s->ev_q_head) {
    s->ev_q[s->ev_q_tail].events = 0;
    s->ev_q[s->ev_q_tail].conn = NULL;
    server_evq_move_tail(s, 1);
    return 1;
  }

  return -1;
}

// takes event at tail and moves it to head
static int server_evq_readd_evqe(server_t *s) {
  conn_t *c = s->ev_q[s->ev_q_tail].conn;
  int events = s->ev_q[s->ev_q_tail].events;

  if (server_evq_delete_evqe(s) == 1) {
    if (server_evq_add_evqe(s, c, events) != NULL) {
      return 0;
    } else {
      return -1;
    }
  };

  return -1;
}

conn_t *server_conn_new(server_t *s, int fd) {
  if (s->num_cons + 1 > MAX_CONNS) {
    return NULL;
  } else {
    assert(s->conns[fd].fd == 0);
    assert(s->conns[fd].off_buf == 0);
    ++s->num_cons;
    s->conns[fd].readable = 1;
    s->conns[fd].writeable = 1;
    return &s->conns[fd];
  }
}

static inline void server_conn_clear(conn_t *c) {
  c->fd = 0;
  c->off_buf = 0;
}

int set_non_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  };

  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return -1;
  };

  return 0;
}

int server_nb_socket_bind_listen() {
  struct addrinfo hints;
  struct addrinfo *servinfo;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if (getaddrinfo(NULL, PORT, &hints, &servinfo) < 0) {
    perror("getaddrinfo");
    exit(1);
  };

  int sfd;
  sfd =
      socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);

  if (sfd < 0) {
    perror("socket()");
    exit(1);
  }

  int on = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));

  if (set_non_blocking(sfd) < 0) {
    perror("set_non_blocking()");
    exit(1);
  }

  if (bind(sfd, servinfo->ai_addr, servinfo->ai_addrlen) < 0) {
    perror("bind()");
    exit(1);
  };

  if (listen(sfd, BACKLOG) < 0) {
    perror("listen()");
    exit(1);
  }

  return sfd;
}

void server_must_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
  if (epoll_ctl(epfd, op, fd, ev) < 0) {
    perror("epoll_ctl()");
    exit(1);
  };
}

void server_event_loop_init(server_t *s) {
  assert(!server_epoll_init(s));

  int epfd = s->ep_fd;
  int server_fd = s->server_fd;
  struct epoll_event ev;

  ev.events = EPOLLIN;
  ev.data.fd = s->server_fd;
  struct sockaddr_storage client_addr;
  socklen_t addr_size;
  addr_size = sizeof client_addr;

  server_must_epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

  int timeout;
  evqe *qe;

  for (;;) {

    // check wether we fully drained the event queue
    // if event queue entry qe is NULL wait forever
    // otherwise set timeout to zero to get immediately ready events
    // and move on to queueing & processing in either case
    if (!(qe = server_evq_peek_evqe(s))) {
      timeout = -1;
    } else {
      timeout = 0;
    }

    // todo(sah): instead of MAX_EVENTS get the space available from event queue
    //            so we don't flood with more events than can be handled
    int n_evs = epoll_wait(s->ep_fd, s->ep_evs, MAX_EVENTS, -1);
    printf("n_evs: %d\n", n_evs);
    // queue up ready events
    for (int i = 0; i < n_evs; ++i) {
      struct epoll_event cur_ev = s->ep_evs[i];
      if (IS_SERVER_FD_EVENT(cur_ev, server_fd)) {
        int client_fd = accept4(server_fd, (struct sockaddr *)&client_addr,
                                &addr_size, O_NONBLOCK);
        if (client_fd < 0) {
          if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
            perror("accept()");
          }
        } else {
          conn_t *c = server_conn_new(s, client_fd);
          assert(c != NULL);
          assert(server_evq_add_evqe(s, c, ERW) != NULL);
          ev.data.fd = client_fd;
          ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
          server_must_epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);
        }

      } else if (cur_ev.events & EPOLLRDHUP) {
        assert(server_evq_add_evqe(s, &s->conns[cur_ev.data.fd], ESHOULDCLOSE));
      } else if (cur_ev.events & EPOLLERR) {
        assert(server_evq_add_evqe(s, &s->conns[cur_ev.data.fd], ESHOULDCLOSE));
      } else if (cur_ev.events & EPOLLHUP) {
        assert(server_evq_add_evqe(s, &s->conns[cur_ev.data.fd], ESHOULDCLOSE));
      } else if (cur_ev.events & EPOLLIN) {
        assert(server_evq_add_evqe(s, &s->conns[cur_ev.data.fd],
                                   s->conns[cur_ev.data.fd].writeable ? ERW
                                                                      : EREAD));
      } else if (cur_ev.events & EPOLLOUT) {
        assert(server_evq_add_evqe(s, &s->conns[cur_ev.data.fd],
                                   s->conns[cur_ev.data.fd].readable ? ERW
                                                                     : EWRITE));
      }
    }

    // proccess ready events
    int to_proccess = server_evq_get_head(s) - server_evq_get_tail(s);
    printf("%d\n", to_proccess);
    while (to_proccess--) {
      printf("proccessing...\n");
      qe = server_evq_peek_evqe(s);
      assert(qe != NULL);
      switch (qe->events) {
      case ERW:
        printf("ERW\n");
        break;
      case EREAD:
        printf("EREAD\n");
        break;
      case EWRITE:
        printf("EWRITE\n");
        break;
      case ESHOULDCLOSE:
        printf("ESHOULDCLOSE\n");
      }

      server_evq_delete_evqe(s);
    }
  }
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  int fd = server_nb_socket_bind_listen();

  server_t *s = server_new(fd);
  server_set_fd(s, fd);
  printf("epoll backed TCP echo server starting on port: %s\n", PORT);

  server_event_loop_init(s);
}
