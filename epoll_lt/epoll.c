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
#include <stdbool.h>
#include <stddef.h>
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

#define LISTEN_BACKLOG 1024 * 4
#define DEFAULT_PORT 9919
#define MAX_EVENTS 1024 * 10
#define BUF_SIZE (1024 * 8)

typedef struct {
  int buf_offset;
  unsigned char buf[BUF_SIZE];
} conn_t;

static inline int conn_readable(conn_t *conn) {
  return BUF_SIZE - conn->buf_offset > 0;
}

static inline int conn_writeable(conn_t *conn) { return conn->buf_offset > 0; }

static inline int conn_buf_get_offset(conn_t *conn) { return conn->buf_offset; }

static inline void conn_buf_offset_inc(conn_t *conn, int d) {
  conn->buf_offset += d;
}

static inline void conn_buf_offset_dec(conn_t *conn, int d) {
  conn->buf_offset -= d;
}

typedef struct {
  conn_t conns[MAX_EVENTS];
} server_t;

int handle_conn(conn_t *conn, int fd, int epoll_fd, struct epoll_event *ev,
                int read) {
  int can_read = 1 & read;
  int can_write = 1;
  int readable = can_read & conn_readable(conn);
  int writeable = can_write & conn_writeable(conn);
  int count;
  int ret = 0;
  int ok = 0;
#define MAX_LOOPS 4

  for (count = 0; count < MAX_LOOPS; ++count) {
    if (writeable) {
      ret = send(fd, conn->buf, conn_buf_get_offset(conn), 0);
      ok = ret > 0;
      conn_buf_offset_dec(conn, (ok * ret));

      if (!ok) {
        if (ret == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
          // printf("paused reading1: %d\n", fd);
          can_write = 0;
          ev->data.fd = fd;
          ev->events = EPOLLOUT | EPOLLRDHUP;
          assert(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, ev) == 0);
        } else {
          return -1;
        }
      }

      // don't try writing, it will EAGAIN
      if (ok & (conn_buf_get_offset(conn) > 0)) {
        // printf("paused reading2: %d\n", fd);
        can_write = 0;
        ev->data.fd = fd;
        ev->events = EPOLLOUT | EPOLLRDHUP;
        assert(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, ev) == 0);
      }
    };

    readable = can_read  & conn_readable(conn);

    if (readable) {
      ret = recv(fd, conn->buf + conn_buf_get_offset(conn),
                 BUF_SIZE - conn_buf_get_offset(conn), 0);
      ok = (ret > 0);

      conn_buf_offset_inc(conn, (ok * ret));
      if (!ok) {
        // perror("recv()");
        if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          return 0;
        } else {
          return -1;
        }
      } else {
        if (conn_buf_get_offset(conn) != BUF_SIZE) {
          can_read = 0;
        }
      }
    }

    writeable = can_write & conn_writeable(conn);

    if (!(readable || writeable)) {
      return 0;
    };
  }

  return 0;
}

int main(void) {
  printf("pid: %d\n", getpid());
  signal(SIGPIPE, SIG_IGN);

  server_t *server = mmap(NULL, sizeof *server, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE | MAP_POPULATE, -1, 0);
  assert(server != MAP_FAILED);

  int server_fd;
  struct sockaddr_in srv_addr;

  server_fd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

  int on = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(DEFAULT_PORT);
  srv_addr.sin_addr.s_addr = htons(INADDR_ANY);

  if (bind(server_fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr))) {
    perror("bind");
    exit(1);
  }

  assert(listen(server_fd, LISTEN_BACKLOG) >= 0);

  if (listen(server_fd, LISTEN_BACKLOG)) {
    perror("listen");
    exit(1);
  }

  printf("listening on port:%d\n", DEFAULT_PORT);

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

  for (;;) {

    int event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    if (event_count < 0) {
      perror("epoll_wait");
      return EXIT_FAILURE;
    }

    // loop over events
    for (int i = 0; i < event_count; ++i) {
      if (events[i].data.fd == server_fd) {
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

          ev.events = EPOLLIN | EPOLLRDHUP;
          ev.data.fd = client_fd;

          server->conns[client_fd].buf_offset = 0;

          assert(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == 0);
        }

      } else {
        int fd = events[i].data.fd;
        // printf("servicing: %d\n", fd);
        conn_t *conn = &server->conns[fd];
        // handle closure
        if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          ev.data.fd = fd;
          assert(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev) == 0);
          assert(close(fd) == 0);
        } else {
          if (events[i].events & EPOLLOUT) {
            // printf("continue reading: %d\n", fd);
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLRDHUP;
            assert(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0);
          }

          int read = 0;
          if (events[i].events & EPOLLIN) {
            read = 1;
          }

          int ret = handle_conn(conn, fd, epoll_fd, &ev, read);
          if (ret == -1) {
            ev.data.fd = fd;
            assert(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev) == 0);
            assert(close(fd) == 0);
          }
        }
      }
    }
  }

  // end of event loop
  close(epoll_fd);
  close(server_fd);
  munmap(server, sizeof *server);

  return EXIT_SUCCESS;
}
