#ifndef CONN_CTX_IO_URING_ECHO
#define CONN_CTX_IO_URING_ECHO

#include <stdint.h>


#define FD_MASK ((1ULL << 21) - 1)
#define BGID_SHIFT 21
#define BGID_MASK (((1ULL << 15) - 1) << BGID_SHIFT)

#define EVENT_SHIFT 36
#define EVENT_MASK (3ULL << EVENT_SHIFT)

#define BUFIDX_SHIFT 39
#define BUFIDX_MASK (((1ULL << 16) - 1) << BUFIDX_SHIFT)



static inline void conn_set_fd(uint_fast64_t *data, uint32_t fd) {
  *data = (*data & ~FD_MASK) | (uint_fast64_t)fd;
}

static inline void conn_set_bgid(uint_fast64_t *data, uint32_t index) {
  *data = (*data & ~BGID_MASK) | ((uint_fast64_t)index << BGID_SHIFT);
}

static inline void conn_set_buf_idx(uint_fast64_t *data, uint32_t index) {
  *data = (*data & ~BUFIDX_MASK) | ((uint_fast64_t)index << BUFIDX_SHIFT);
}

static inline void conn_set_event(uint_fast64_t *data, uint8_t event) {
  *data = (*data & ~EVENT_MASK) | ((uint_fast64_t)event << EVENT_SHIFT);
}

static inline uint32_t conn_get_fd(uint_fast64_t data) { return (data & FD_MASK); }

static inline uint32_t conn_get_bgid(uint_fast64_t data) {
  return (data & BGID_MASK) >> BGID_SHIFT;
}

static inline uint32_t conn_get_buf_idx(uint_fast64_t data) {
  return (data & BUFIDX_MASK) >> BUFIDX_SHIFT;
}

static inline uint8_t conn_get_event(uint_fast64_t data) {
  return (data & EVENT_MASK) >> EVENT_SHIFT;
}

#endif /* CONN_CTX_IO_URING_ECHO */

