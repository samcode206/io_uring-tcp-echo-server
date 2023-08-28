#define _GNU_SOURCE
#include "conn.h"
#include "evq.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define LISTEN_BACKLOG 1024
#define DEFAULT_PORT "9919"

typedef struct {
  conn_t conns[MAX_EVENTS];
  evq_t evq;
} server_t;

int server_handle_conn(server_t *server, conn_t *conn, int epoll_fd,
                       struct epoll_event *ev);

static void evq_proccess_events(server_t *server, int epollfd,
                                struct epoll_event *ev) {
  size_t ev_count = MAX_EVENTS - evq_get_space(&server->evq) - 1;

  conn_t *conn;
  int fd;

  while (ev_count-- > 0) {
    conn = (conn_t *)evq_peek_evqe(&server->evq);
    if (conn == 0) {
      evq_delete_evqe(&server->evq);
    } else {
      fd = (int)conn_ctx_get_fd(conn);
      if (!fd) {
        evq_delete_evqe(&server->evq);
        continue;
      }

      int ret = server_handle_conn(server, conn, epollfd, ev);
      if (ret) {
        server_evq_readd_evqe(&server->evq);
      } else if (ret == -1) {
        conn_clear(conn);
        ev->data.fd = fd;
        assert(epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, ev) == 0);
        assert(close(fd) == 0);
        evq_delete_evqe(&server->evq);
      } else {
        conn_ctx_unset_ev(conn, CONN_QUEUED);
        evq_delete_evqe(&server->evq);
      }
    }
  }
}

int server_handle_conn(server_t *server, conn_t *conn, int epoll_fd,
                       struct epoll_event *ev) {
  int fd = conn_ctx_get_fd(conn);
  int readable = conn_readable(conn, BUFF_CAP);
  int writeable = conn_writeable(conn);
  int count;
  int ret = 0;
  int ok = 0;

  for (count = 0; count < 4; ++count) {
    if (writeable) {
      ret = send(fd, conn->buf, conn_ctx_get_buff_offset(conn), 0);
      ok = ret > 0;
      conn_ctx_dec_buff_offset(conn, (ok * ret));

      if (!ok) {
        if (ret == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
          // perror("send()");
          conn_ctx_unset_ev(conn, ECONN_WRITEABLE);
          ev->data.fd = fd;
          ev->events = EPOLLOUT | EPOLLRDHUP | EPOLLET;
          assert(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, ev) == 0);
        } else {
          return -1;
        }
      }

      // don't try writing, it will EAGAIN
      if (conn_ctx_get_buff_offset(conn)) {
        conn_ctx_unset_ev(conn, ECONN_WRITEABLE);
        ev->data.fd = fd;
        ev->events = EPOLLOUT | EPOLLRDHUP | EPOLLET;
        assert(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, ev) == 0);
      }
    };

    readable = conn_readable(conn, BUFF_CAP);

    if (readable) {
      ret = recv(fd, conn->buf + conn_ctx_get_buff_offset(conn),
                 BUFF_CAP - conn_ctx_get_buff_offset(conn), 0);
      ok = (ret > 0);
      conn_ctx_inc_buff_offset(conn, (ok * ret));

      if (!ok) {
        if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          // perror("recv()");
          conn_ctx_unset_ev(conn, ECONN_READABLE);
        } else {
          return -1;
        }
      }
    }

    writeable = conn_writeable(conn);

    if (!(readable || writeable)) {
      return 0;
    };
  }

  return 1;
}

int main(int argc, char *argv[]) {

  signal(SIGPIPE, SIG_IGN);

  server_t *server = mmap(NULL, sizeof *server, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE | MAP_POPULATE, -1, 0);
  assert(server != MAP_FAILED);

  char *port = DEFAULT_PORT;
  if (argc > 1) {
    port = argv[1];
  }

  // set up addr info
  struct addrinfo hints;
  struct addrinfo *servinfo;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if (getaddrinfo(NULL, port, &hints, &servinfo) < 0) {
    perror("getaddrinfo");
    return EXIT_FAILURE;
  }

  // set up socket
  int server_fd =
      socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
  if (server_fd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }

  // make socket non blocking
  int curr_flags = fcntl(server_fd, F_GETFL, 0);
  if (curr_flags < 0) {
    perror("fcntl");
    return EXIT_FAILURE;
  }

  if (fcntl(server_fd, F_SETFL, curr_flags | O_NONBLOCK) < 0) {
    perror("fcntl");
    return EXIT_FAILURE;
  }

  int on = 1;
  assert(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int)) ==
         0);

  // bind socket
  if (bind(server_fd, servinfo->ai_addr, servinfo->ai_addrlen) < 0) {
    perror("bind");
    return EXIT_FAILURE;
  };

  // listen for connections
  if (listen(server_fd, LISTEN_BACKLOG) < 0) {
    perror("listen");
    return EXIT_FAILURE;
  }

  printf("listening on port:%s\n", port);

  // set up epoll
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1");
    return EXIT_FAILURE;
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
    perror("epoll_ctl");
    return EXIT_FAILURE;
  };

  struct epoll_event events[MAX_EVENTS];
  struct sockaddr_storage client_sockaddr;
  socklen_t client_socklen;
  client_socklen = sizeof client_sockaddr;

  // start of event loop
  for (;;) {
    int timeout = 0;
    if (evq_is_empty(&server->evq)) {
      timeout = -1;
    };

    int event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout);
    if (event_count < 0) {
      // todo when and why would this fail and can we recover?
      perror("epoll_wait");
      return EXIT_FAILURE;
    }

    // loop over events
    for (int i = 0; i < event_count; ++i) {
      if (events[i].data.fd == server_fd) {
        // new clients are coming in spin accepting all connections
        for (;;) {
          int client_fd =
              accept4(server_fd, (struct sockaddr *)&client_sockaddr,
                      &client_socklen, O_NONBLOCK);
          if (client_fd < 0) {
            if (!(errno == EAGAIN)) {
              perror("accept");
            }
            break;
          }

          if (client_fd > MAX_EVENTS - 5) {
            printf("can't index fd: %d\n", client_fd);
            close(client_fd);
            continue;
          }

          assert(server->conns[client_fd].ctx == 0);
          conn_ctx_set_fd(&server->conns[client_fd], client_fd);
          conn_ctx_set_ev(&server->conns[client_fd],
                          ECONN_READABLE | ECONN_WRITEABLE);

          // add to epoll struct with events
          ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
          ev.data.fd = client_fd;
          assert(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == 0);
        }

      } else {
        int fd = events[i].data.fd;
        // printf("servicing: %d\n", fd);
        conn_t *conn = &server->conns[fd];
        // handle closure
        if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          // printf("closing: %d\n", fd);
          ev.data.fd = fd;
          assert(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev) == 0);
          assert(close(fd) == 0);
          conn_clear(conn);
        } else {
          if (events[i].events & EPOLLOUT) {
            conn_ctx_set_ev(conn, ECONN_WRITEABLE);
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
            assert(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0);
          }

          if (events[i].events & EPOLLIN) {
            conn_ctx_set_ev(conn, ECONN_READABLE);
          }

          if (!conn_ctx_check_ev(conn, CONN_QUEUED)) {
            int ret = server_handle_conn(server, conn, epoll_fd, &ev);
            if (ret == -1) {
              ev.data.fd = fd;
              assert(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev) == 0);
              assert(close(fd) == 0);
              conn_clear(conn);
            } else if (ret) {
              conn_ctx_set_ev(conn, CONN_QUEUED);
              evq_add_evqe(&server->evq, (uint64_t)conn);
            };
          }
        }
      }
    }

    evq_proccess_events(server, epoll_fd, &ev);
  }

  // end of event loop
  freeaddrinfo(servinfo);
  close(epoll_fd);
  close(server_fd);
  munmap(server, sizeof *server);

  return EXIT_SUCCESS;
}
