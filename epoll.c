#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
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
void server_accept_epoll_ctl_add(int server_fd, int epoll_fd,
                                 struct epoll_event *ev,
                                 struct sockaddr *client_addr,
                                 socklen_t addr_size) {
  int fd = accept(server_fd, client_addr, &addr_size);
  if (fd < 0) {
    perror("accept()");
    exit(1);
  }

  if (set_non_blocking(fd) < 0) {
    perror("set_non_blocking()");
    exit(1);
  }

  ev->events = EPOLLIN | EPOLLET | EPOLLRDHUP;
  ev->data.fd = fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, ev) < 0) {
    perror("epoll_ctl()");
    exit(1);
  };
}

void server_start(int sfd) {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1()");
    exit(1);
  }

  char data[BUF_SZ];
  struct epoll_event events[MAX_EVENTS];
  events[0].events = EPOLLIN;
  events[0].data.fd = sfd;
  struct sockaddr_storage client_addr;
  socklen_t addr_size;
  addr_size = sizeof client_addr;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sfd, &events[0]) < 0) {
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
      if (events[i].data.fd == sfd) {
        server_accept_epoll_ctl_add(sfd, epoll_fd, &events[i],
                                    (struct sockaddr *)&client_addr, addr_size);
      } else {
        if (events[i].events & EPOLLIN) {
          ssize_t n = recv(events[i].data.fd, data, BUF_SZ, 0);
          if (n <= 0) {
            if (n == EAGAIN || n == EWOULDBLOCK) {
              break;
            }
            if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd,
                          &events[i]) < 0) {
              perror("epoll_ctl()");
              exit(1);
            };

            close(events[i].data.fd);
          } else {
            int ret = send(events[i].data.fd, data, n, MSG_DONTWAIT);
            if (ret <= 0) {
              perror("send()");
            }
          }
        } else if ((events[i].events & EPOLLRDHUP) ||
                   (events[i].events & EPOLLHUP)) {
          if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd,
                        &events[i]) < 0) {
            perror("epoll_ctl()");
            exit(1);
          };

          close(events[i].data.fd);
        }
      }
    };
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
