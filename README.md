# io_uring-tcp-echo-server

[RFC862 TCP Echo Server](https://www.rfc-editor.org/rfc/rfc862)

- Linux Kernel 6.1 >= required
- [liburing](https://github.com/axboe/liburing)


This Repo contains two TCP echo server implementations, the purpose is to find out how linux's io_uring performs under heavy network io workloads compared to the well established and proven epoll.

## io_uring

the io_uring implementation uses features that gave the best performance for me when i tested out, this however doesn't include multishot recv as I found that to be hard to control and was prone to starvation when there was more than one connection, for this reason multi shot recv is not used, multi shot accept, direct file descriptors however are used and did not negatively affect performance.


## epoll

the epoll echo server implementation uses edge triggered events and a ring buffer based queue to provide a full robust echo server implementation for a fair performance comparison, the way it works is simple, we wait for readiness by calling `epoll_wait` once some io is possible on some connections we attempt to do `recv/send`s until either we reach the set limit for number of io operations per connection or we get `EAGAIN` if we don't get `EAGAIN` we place the fd in the ring buffer and move on to the next connection, we then proceed to do more I/O for the still ready fds in the ring buffer, 
if more I/O is possible on an fd it gets re-added to the queue otherwise it is removed, we then repeat the cycle, this is done to ensure we aren't causing any starvation and give each connection a fair time and to still be able to take advantage of edge triggered notifications.


## benchmarks

echo server benchmarks were done in two different way to simulate two common Network IO workloads:

- streaming client (client continuously writes data without waiting for an echo)
- request-response client (client writes data waits for response before beginning next write)


io_uring does better in request-response type of workloads but struggled to do as good when client is streaming data here are some results with different payload sizes

### Streaming client


### Request response




