# IO_uring vs.Epoll: A Comparative Study of TCP Echo Server Performance

This repository houses a comparison between two TCP Echo Server implementations, adhering to [RFC862](https://www.rfc-editor.org/rfc/rfc862). The primary objective is to assess the performance of Linux's io_uring under intense network I/O workloads in contrast to the well-established epoll mechanism.


## Prerequisites
- [liburing](https://github.com/axboe/liburing)
- Linux Kernel version 6.1 or above (earlier versions may work but are untested)


## epoll Implementation

The epoll-based TCP Echo Server is designed to use edge-triggered events and employs a ring-buffer queue to cache ready fds that still have IO possible. The operational flow is straightforward:
- Await I/O readiness via the epoll_wait call.
- Once the system signals I/O readiness on certain connections, multiple recv/send operations are performed until either a set limit is reached or an EAGAIN error occurs.
- If the EAGAIN error is not triggered, the corresponding file descriptor (fd) is placed into the ring buffer, and we proceed to the next available connection.
- Additional I/O is performed on the remaining ready fds stored in the ring buffer.

This iterative process ensures that each connection receives fair processing time while taking advantage of edge-triggered notifications.
## io_uring

the io_uring implementation uses features that gave a mix of good performance and stability/fairness, this however doesn't include `multishotrecv` as I found that to be hard to control against one connection monopolizing mapped provided buffers (esp in streaming mode) and was prone to starvation when there was more than one connection (some connections got a lot more time at the cost of others, this was only discoverable thanks to my benchmarking tool having per connection metrics), for this reason multi shot recv is not used, multi shot accept, direct file descriptors however are used and did not negatively affect performance.

flags that were used included `IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN |IORING_SETUP_SINGLE_ISSUER`

- Arm multishot accept and wait for connections
- when connections arrive submit a recv request for each connection accepted 
- when data is read from the set of connections into the ring mapped buffers submit a request to echo back the data
- when a completion occurs for the send requests release the buffers back to the kernel and submit a recv request 

## benchmarks

Benchmark tests were conducted in two different scenarios to simulate common Network I/O workloads:

- Streaming Client: The client continuously writes data without waiting for an echo.
- Request-Response Client: The client writes data, waits for a response, and then initiates the next write.

### Observations 

io_uring outperforms epoll in request-response workloads but faces challenges in keeping up with epoll when clients are streaming data.

### Streaming client

![256 byte payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/256/256.png?raw=true)

![512 byte payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/512/512.png?raw=true)

![1kb payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/1024/1kb.png?raw=true)

![4kb payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/4096/4kb.png?raw=true)


### Request-response

![256 byte req-res payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/req-res/256/256-req-res.png?raw=true)

![512 byte req-res payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/req-res/512/512-req-res.png?raw=true)



To Run either server locally

1. Ensure that liburing is installed.
2. Adjust the buffer and max event/max connection settings in the source code for each server as necessary.

These tests were executed on a `11th Gen Intel® Core™ i9-11900K @ 3.50GHz` debian 12 (running directly on hardware no vm), with the servers pinned to CPU 15 via `taskset -cp 15 {{pid}}`. The kernel parameters were set as `mitigations=off isolcpus=15`.

