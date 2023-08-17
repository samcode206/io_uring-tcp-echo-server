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

#define MAX_EVENTS 1024

#define IS_SERVER_FD_EVENT(e, sfd) ((e).data.fd == (sfd))

struct conn {
  int fd;

  bool readable;
  bool writeable;

  ssize_t off_buf;
  char buf[BUF_SZ];
};

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

// accept connection and add to epoll interest list
struct conn *server_accept_epoll_ctl_add(int server_fd, int epoll_fd,
                                         struct epoll_event *ev,
                                         struct sockaddr *client_addr,
                                         socklen_t addr_size) {
  int fd = accept(server_fd, client_addr, &addr_size);
  if (fd < 0) {
    perror("accept()");
    exit(1);
  }

  struct conn *c = malloc(sizeof(struct conn));
  if (!c) {
    printf("out of memory\n");
    exit(1);
  }

  c->fd = fd;
  c->readable = 1;
  c->writeable = 1;
  c->off_buf = 0;

  if (set_non_blocking(fd) < 0) {
    perror("set_non_blocking()");
    exit(1);
  }

  ev->events = EPOLLIN | EPOLLET;
  ev->data.ptr = c;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, ev) < 0) {
    perror("epoll_ctl()");
    exit(1);
  };

  return c;
}

void conn_echo_all(int epfd, struct epoll_event *ev) {
  struct conn *c = ev->data.ptr;
  c->readable = 1;
  while (c->readable || (c->off_buf && c->writeable)) {
    if (c->readable) {
      int nr = recv(c->fd, c->buf + c->off_buf, BUF_SZ - c->off_buf, 0);
      if (nr <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          // done for now
          c->readable = 0;
        } else {
          // close
          perror("recv()");
          exit(1);
        }
      } else {
        c->off_buf += nr;
      }
    }

    if (c->off_buf && c->writeable) {
      int nw = send(c->fd, c->buf, c->off_buf, 0);
      if (nw <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          c->writeable = 0;

          ev->events = EPOLLOUT | EPOLLET;
          if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, ev) < 0) {
            perror("epoll_ctl()");
            exit(1);
          };

        } else {
          // close
          perror("send()");
          exit(1);
        }
      } else {
        c->off_buf -= nw;
        // printf("wrote: %d left %zu\n", nw, c->off_buf);
      }
    }
  }
}

void conn_resume_send(int epfd, struct epoll_event *ev) {
  struct conn *c = ev->data.ptr;
  c->writeable = 1;

  bool enable_read = 1;

  while (c->off_buf && c->writeable) {
    int nw = send(c->fd, c->buf, c->off_buf, 0);
    if (nw <= 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        c->writeable = 0;
        enable_read = 0;
        ev->events = EPOLLOUT | EPOLLET;
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, ev) < 0) {
          perror("epoll_ctl()");
          exit(1);
        };

      } else {
        // close
        perror("send()");
        exit(1);
      }
    } else {
      c->off_buf -= nw;
      // printf("wrote: %d left %zu\n", nw, c->off_buf);
    }
  }

  if (enable_read) {
    ev->events = EPOLLIN | EPOLLET;
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, ev);
  }
}

void server_start(int sfd) {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1()");
    exit(1);
  }

  struct epoll_event events[MAX_EVENTS];

  struct epoll_event ev;

  ev.events = EPOLLIN;
  ev.data.fd = sfd;
  struct sockaddr_storage client_addr;
  socklen_t addr_size;
  addr_size = sizeof client_addr;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sfd, &ev) < 0) {
    perror("epoll_ctl()");
    exit(1);
  };

  for (;;) {
    int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    if (n_events < 0) {
      perror("epoll_wait()");
      exit(1);
    }

    for (int i = 0; i < n_events; ++i) {
      if (IS_SERVER_FD_EVENT(events[i], sfd)) {
        server_accept_epoll_ctl_add(sfd, epoll_fd, &ev,
                                    (struct sockaddr *)&client_addr, addr_size);
      } else if (events[i].events & EPOLLHUP) {
        printf("EPOLLHUP\n");
      } else if (events[i].events & EPOLLIN) {
        conn_echo_all(epoll_fd, &events[i]);
      } else if (events[i].events & EPOLLOUT) {
        conn_resume_send(epoll_fd, &events[i]);
      }
    }
  }

  printf("shutting down...\n");
  close(epoll_fd);
  close(sfd);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  int fd = server_nb_socket_bind_listen();
  printf("epoll backed TCP echo server starting on port: %s\n", PORT);
  server_start(fd);
}
