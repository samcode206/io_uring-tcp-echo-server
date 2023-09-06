# io_uring-tcp-echo-server

[RFC862 TCP Echo Server](https://www.rfc-editor.org/rfc/rfc862)

- [liburing](https://github.com/axboe/liburing)
- Linux Kernel 6.1 >= required (earlier should work but untested.)

This Repo contains two TCP echo server implementations, the purpose is to find out how linux's io_uring performs under heavy network io workloads compared to the well established and proven epoll.

## epoll

the epoll echo server implementation uses edge triggered events and a ring buffer based queue to provide a full robust echo server implementation for a fair performance comparison, the way it works is simple, we wait for readiness by calling `epoll_wait` once some io is possible on some connections we attempt to do `recv/send`s until either we reach the set limit for number of io operations per connection or we get `EAGAIN` if we don't get `EAGAIN` we place the fd in the ring buffer and move on to the next connection, we then proceed to do more I/O for the still ready fds in the ring buffer, 
if more I/O is possible on an fd it gets re-added to the queue otherwise it is removed, we then repeat the cycle, this is done to ensure we aren't causing any starvation and give each connection a fair time and to still be able to take advantage of edge triggered notifications.

## io_uring

the io_uring implementation uses features that gave a mix of good performance and stability, this however doesn't include multishotrecv as I found that to be hard to control against one connection monopolizing provided bufferes (esp in streaming mode) and was prone to starvation when there was more than one connection (some connections got a lot more time at the cost of others, this was only discoverable thanks to my benchmarking tool having per connection metrics), for this reason multi shot recv is not used, multi shot accept, direct file descriptors however are used and did not negatively affect performance.

## benchmarks

echo server benchmarks were done in two different way to simulate two common Network IO workloads:

- streaming client (client continuously writes data without waiting for an echo)
- request-response client (client writes data waits for response before beginning next write)


io_uring does better in request-response type of workloads but struggled to do as good when client is streaming data here are some results with different payload sizes

### Streaming client

![256 byte payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/256/256.png?raw=true)

![512 byte payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/512/512.png?raw=true)

![1kb payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/1024/1kb.png?raw=true)

![4kb payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/4096/4kb.png?raw=true)


### Request-response

![256 byte req-res payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/req-res/256/256-req-res.png?raw=true)

![512 byte req-res payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/req-res/512/512-req-res.png?raw=true)


`/bench` has the raw results with per client metrics 

to run those tests locally ensure you have liburing installed and adjust the buffer/max events sizing in the source code for each server as needed. 
these servers were both ran with `taskset -cp 15 {{pid}}` no other processes running on 15. kernel parameters were `mitigations=off isolcpus=15` 

the tests were ran locally to eliminate external networking factors on an `11th Gen Intel(R) Core(TM) i9-11900K @ 3.50GHz`



