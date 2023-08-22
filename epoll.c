/*
MIT License

Copyright (c) 2023 Sam, H

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/
#define _GNU_SOURCE
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

#define WITH_ASSERTIONS 0

#define PORT "9919"
#define BACKLOG 1024
#define BUF_SZ 1024 * 128

#define MAX_EVENTS 1024
#define MAX_CONNS 1024

#define ECONN_READABLE (1 << 0)  // 0001
#define ECONN_WRITEABLE (1 << 1) // 0010

#define ECONN_SHOULD_CLOSE (1 << 2) // 0100

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct {
  int events;
  int fd;
  int n_qe;
  ssize_t off_buf;
  char buf[BUF_SZ];
} conn_t;

typedef struct {
  int server_fd;
  int ep_fd;
  size_t num_cons;

  size_t ev_q_head;
  size_t ev_q_tail;
  conn_t *ev_q[MAX_EVENTS];
  conn_t conns[MAX_CONNS];
  struct epoll_event ep_evs[MAX_EVENTS];

} server_t;

server_t *server_new() {
  server_t *s = calloc(1, sizeof(server_t));
  return s;
}

int server_epoll_init(server_t *s) {
  int fd = epoll_create1(0);
  if (fd < 0) {
    return fd;
  }
  s->ep_fd = fd;
  return 0;
}

static inline void server_set_fd(server_t *s, int fd) { s->server_fd = fd; }

static inline int server_get_fd(server_t *s) { return s->server_fd; }

static inline size_t server_evq_get_head(server_t *s) {
  return s->ev_q_head & (MAX_EVENTS - 1);
}

static inline size_t server_evq_get_tail(server_t *s) {
  return s->ev_q_tail & (MAX_EVENTS - 1);
}

static inline size_t server_evq_get_space(server_t *s) {
  return MAX_EVENTS - 1 - ((s->ev_q_head - s->ev_q_tail) & (MAX_EVENTS - 1));
}

static inline void server_evq_move_head(server_t *s, int d) {
  s->ev_q_head = s->ev_q_head + d;
}

static inline void server_evq_move_tail(server_t *s, int d) {
  s->ev_q_tail = s->ev_q_tail + d;
}

static inline bool server_evq_is_empty(server_t *s) {
  if ((server_evq_get_tail(s) == server_evq_get_head(s))) {
    return 1;
  }
  return 0;
}

static inline conn_t *server_evq_peek_evqe(server_t *s) {
  if (LIKELY(server_evq_get_head(s) != server_evq_get_tail(s))) {
    return s->ev_q[server_evq_get_tail(s)];
  }

  return NULL;
}

static conn_t *server_evq_add_evqe(server_t *s, conn_t *c) {
  if (LIKELY(server_evq_get_space(s) > 0)) {
    s->ev_q[server_evq_get_head(s)] = c;
    server_evq_move_head(s, 1);
    ++c->n_qe;
#if WITH_ASSERTIONS
    assert(c->n_qe == 1);
#endif

    return c;
  } else {
    return NULL;
  }
}

static void server_evq_delete_evqe(server_t *s) {
  size_t tail = server_evq_get_tail(s);

#if WITH_ASSERTIONS
  assert(s->ev_q[tail] != NULL);
  assert(s->ev_q[tail]->n_qe == 1);
#endif

  s->ev_q[tail]->n_qe = 0;
  s->ev_q[tail] = NULL;
  server_evq_move_tail(s, 1);
}

// takes event at tail and moves it to head
static void server_evq_readd_evqe(server_t *s) {
  conn_t *c = s->ev_q[server_evq_get_tail(s)];

  size_t tail = server_evq_get_tail(s);
  s->ev_q[tail]->n_qe = 0;
  s->ev_q[tail] = NULL;
  server_evq_move_tail(s, 1);

  s->ev_q[server_evq_get_head(s)] = c;
  server_evq_move_head(s, 1);
  ++c->n_qe;
}

void conn_set_event(conn_t *c, int ev_mask) { c->events |= ev_mask; }

void conn_unset_event(conn_t *c, int ev_mask) { c->events &= ~ev_mask; }

bool conn_check_event(conn_t *c, int ev_mask) {
  return (c->events & ev_mask) != 0;
}

conn_t *server_conn_new(server_t *s, int fd) {
  if (UNLIKELY(s->num_cons + 1 >= MAX_CONNS)) {
    return NULL;
  } else {

#if WITH_ASSERTIONS
    assert(s->conns[fd].fd == 0);
    assert(s->conns[fd].off_buf == 0);
    assert(s->conns[fd].events == 0);
    assert(s->conns[fd].n_qe == 0);
#endif

    ++s->num_cons;
    s->conns[fd].fd = fd;
    conn_set_event(&s->conns[fd], ECONN_READABLE);
    conn_set_event(&s->conns[fd], ECONN_WRITEABLE);
    return &s->conns[fd];
  }
}

static inline void server_conn_clear(server_t *s, conn_t *c) {
  memset(c, 0, offsetof(conn_t, buf));
  --s->num_cons;
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

/* conn_can_read returns true only if conn_t c has ECONN_READABLE event set and
 * there's space in c->buf to write into */
static inline bool conn_should_read(conn_t *c) {
  return conn_check_event(c, ECONN_READABLE) && (BUF_SZ - c->off_buf > 0);
}

/* conn_can_write returns true only if conn_t c has ECONN_WRITEABLE event set
 * and there's data to be flushed in c->buf */
static inline bool conn_should_write(conn_t *c) {
  return conn_check_event(c, ECONN_WRITEABLE) && c->off_buf > 0;
}

static void server_must_conn_close(server_t *s, conn_t *c,
                                   struct epoll_event *ev) {
#if WITH_ASSERTIONS
  assert(c->fd != 0);
#endif

  ev->data.fd = c->fd;
  server_must_epoll_ctl(s->ep_fd, EPOLL_CTL_DEL, c->fd, ev);

  if (UNLIKELY(close(c->fd) < 0)) {
    perror("close()");
    exit(1);
  }
}

void server_event_loop_init(server_t *s) {
  if (UNLIKELY(server_epoll_init(s) != 0)) {
    perror("server_epoll_init: failed\n");
    exit(1);
  }

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
  conn_t *qe;

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

    size_t max_evs = server_evq_get_space(s);

    int n_evs = epoll_wait(s->ep_fd, s->ep_evs, max_evs, timeout);

    // queue up ready events
    for (int i = 0; i < n_evs; ++i) {
      struct epoll_event cur_ev = s->ep_evs[i];
      if (cur_ev.data.fd == server_fd) {
        // TODO(sah): make sure to limit to number of conns that we can accept
        while (1) {
          int client_fd = accept4(server_fd, (struct sockaddr *)&client_addr,
                                  &addr_size, O_NONBLOCK);

          if (client_fd < 0) {
            if (!(errno == EAGAIN)) {
              perror("accept4()");
            } else {
              break;
            }
          } else {
            if (UNLIKELY(client_fd > MAX_CONNS - 4)) {
              printf("accept4(): cannot index fd: %d\n", client_fd);
              exit(1);
            }
            conn_t *c = server_conn_new(s, client_fd);
            if (UNLIKELY(c == NULL)) {
              printf("can't accept new connections: server_conn_new\n");
              exit(1);
            }

            ev.data.fd = client_fd;
            ev.events = EPOLLIN | EPOLLET;
            server_must_epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);
          }
        }

      } else if (cur_ev.events & EPOLLRDHUP || cur_ev.events & EPOLLERR ||
                 cur_ev.events & EPOLLHUP) {
        conn_t *c = &s->conns[cur_ev.data.fd];
#if WITH_ASSERTIONS
        assert(c->fd == cur_ev.data.fd);
#endif

        c->events = 0;
        conn_set_event(c, ECONN_SHOULD_CLOSE);
        if (!c->n_qe) {
          if (UNLIKELY(server_evq_add_evqe(s, c) == NULL)) {
            printf("server_evq_add_evqe: evq is full\n");
            exit(1);
          };
        }
      } else {
        if (cur_ev.events & EPOLLOUT) {
          conn_t *c = &s->conns[cur_ev.data.fd];
#if WITH_ASSERTIONS
          assert(c->fd == cur_ev.data.fd);
#endif
          conn_set_event(c, ECONN_WRITEABLE);
          if (!c->n_qe) {
            if (UNLIKELY(server_evq_add_evqe(s, c) == NULL)) {
              printf("server_evq_add_evqe: evq is full\n");
              exit(1);
            };
          }
        }
        if (cur_ev.events & EPOLLIN) {
          conn_t *c = &s->conns[cur_ev.data.fd];
#if WITH_ASSERTIONS
          assert(c->fd == cur_ev.data.fd);
#endif
          conn_set_event(c, ECONN_READABLE);
          if (!c->n_qe) {
            if (UNLIKELY(server_evq_add_evqe(s, c) == NULL)) {
              printf("server_evq_add_evqe: evq is full\n");
              exit(1);
            };
          }
        }
      }
    }

    // proccess ready events
    int to_proccess = MAX_EVENTS - server_evq_get_space(s) - 1;
    // printf("to proccess: %d\n", to_proccess);
    while (to_proccess--) {
      qe = server_evq_peek_evqe(s);
#if WITH_ASSERTIONS
      assert(qe != NULL);
#endif

      // connection was closed and cleared
      if (qe->fd == 0) {
        size_t tail = server_evq_get_tail(s);
#if WITH_ASSERTIONS
        assert(s->ev_q[tail] != NULL);
        assert(s->ev_q[tail]->n_qe == 0);
        assert(s->ev_q[tail]->fd == 0);
        assert(s->ev_q[tail]->off_buf == 0);
#endif

        s->ev_q[tail]->n_qe = 0;
        s->ev_q[tail] = NULL;
        server_evq_move_tail(s, 1);
      } else {
        if (conn_check_event(qe, ECONN_SHOULD_CLOSE)) {

          server_must_conn_close(s, qe, &ev);
          server_evq_delete_evqe(s);
          server_conn_clear(s, qe);

        } else {
          if (!conn_check_event(qe, ECONN_READABLE)) {
            ev.data.fd = qe->fd;
            ev.events = EPOLLIN | EPOLLET;
            server_must_epoll_ctl(epfd, EPOLL_CTL_MOD, ev.data.fd, &ev);
          }
          bool readable =
              conn_check_event(qe, ECONN_READABLE) && BUF_SZ - qe->off_buf > 0;
          bool writeable =
              conn_check_event(qe, ECONN_WRITEABLE) && qe->off_buf > 0;

          int i = 0;
          ssize_t send_ret = -1;
          ssize_t recv_ret = -1;

#define MAX_LOOPS 16
          while (1) {
            if (!readable && !writeable) {
              server_evq_delete_evqe(s);
              break;
            } else if (i++ == MAX_LOOPS) {
              server_evq_readd_evqe(s);
              break;
            }

            if (writeable) {
              send_ret = send(qe->fd, qe->buf, qe->off_buf, 0);

              if (send_ret > 0) {
                qe->off_buf -= send_ret;
              } else {
                conn_unset_event(qe, ECONN_WRITEABLE);
                conn_unset_event(qe, ECONN_READABLE);
                if (send_ret == -1 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                  // stop reading more data and rearm EPOLLOUT

                  ev.data.fd = qe->fd;
                  ev.events = EPOLLOUT | EPOLLET;
                  server_must_epoll_ctl(epfd, EPOLL_CTL_MOD, ev.data.fd, &ev);
                  server_evq_delete_evqe(s);
                } else {
                  // close & clean up
                  server_must_conn_close(s, qe, &ev);
                  server_evq_delete_evqe(s);
                  server_conn_clear(s, qe);
                }
                break; // breakout of loop regardless of error
              }
            }

            readable = conn_check_event(qe, ECONN_READABLE) &&
                       BUF_SZ - qe->off_buf > 0;

            if (readable) {
              recv_ret =
                  recv(qe->fd, qe->buf + qe->off_buf, BUF_SZ - qe->off_buf, 0);

              if (recv_ret > 0) {
                qe->off_buf += recv_ret;
              } else {
                conn_unset_event(qe, ECONN_READABLE);

                if (recv_ret == -1 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                  // perror("is_tmp_io_err::recv()");
                  server_evq_delete_evqe(s);
                } else {
                  // close & clean up
                  server_must_conn_close(s, qe, &ev);
                  server_evq_delete_evqe(s);
                  server_conn_clear(s, qe);
                }
                break; // // breakout of loop regardless of error
              }
            }

            writeable =
                conn_check_event(qe, ECONN_WRITEABLE) && qe->off_buf > 0;
          }
        }
      }
    }
  }
}

void sig_int_handler(int sig) { exit(0); }

int main(void) {
  static_assert((MAX_EVENTS & (MAX_EVENTS - 1)) == 0,
                "MAX_EVENTS must be a power of 2");
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, sig_int_handler);
  int fd = server_nb_socket_bind_listen();

  server_t *s = server_new();
  server_set_fd(s, fd);
  printf("epoll backed TCP echo server starting on port: %s\n", PORT);

  server_event_loop_init(s);
}
