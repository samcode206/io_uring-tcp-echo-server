#ifndef __CONN_CTX_CLT
#define __CONN_CTX_CLT

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BUFF_CAP 1024 * 8

typedef  struct {
  uint_fast64_t ctx;
  char buf[BUFF_CAP];
} conn_t;



#define FD_MASK ((1ULL << 21) - 1)

#define BUFF_OFFSET_MASK (((1ULL << 32) - 1) << 21)
#define BUFF_OFFSET_SHIFT 21

#define ECONN_READABLE (1ULL << 54)
#define ECONN_WRITEABLE (1ULL << 55)

#define CONN_QUEUED (1ULL << 56)

static inline uint32_t conn_ctx_get_fd(conn_t *conn) {
  return conn->ctx & FD_MASK;
}

static inline uint32_t conn_ctx_get_buff_offset(conn_t *conn) {
  return (conn->ctx & BUFF_OFFSET_MASK) >> BUFF_OFFSET_SHIFT;
}

static inline void conn_ctx_set_fd(conn_t * conn, uint32_t fd) {
  conn->ctx = (conn->ctx & ~FD_MASK) | (uint64_t)fd;
}

static inline void conn_ctx_set_buff_offset(conn_t *conn, uint32_t off) {
  conn->ctx = (conn->ctx & ~BUFF_OFFSET_MASK) | (uint64_t)off << BUFF_OFFSET_SHIFT;
}

static inline void conn_ctx_inc_buff_offset(conn_t *conn, uint32_t d) {
  conn_ctx_set_buff_offset(conn, conn_ctx_get_buff_offset(conn) + d);
}

static inline void conn_ctx_dec_buff_offset(conn_t *conn, uint32_t d) {
  conn_ctx_set_buff_offset(conn, conn_ctx_get_buff_offset(conn) - d);
}


static inline void conn_ctx_clear(conn_t *conn) { conn->ctx = 0; }

static inline int conn_ctx_check_ev(conn_t *conn, uint64_t e) {
  return (conn->ctx & e) != 0;
}

static inline void conn_ctx_set_ev(conn_t *conn, uint64_t e) { conn->ctx |= e; }

static inline void conn_ctx_unset_ev(conn_t *conn, uint64_t e) {
  conn->ctx &= ~e;
}

static inline void conn_ctx_clear_evs(conn_t *ctx) {
  conn_ctx_unset_ev(ctx, ECONN_READABLE | ECONN_WRITEABLE | CONN_QUEUED);
}

static inline int conn_readable(conn_t* conn, size_t cap) {
  return conn_ctx_check_ev(conn, ECONN_READABLE) &
         (cap - conn_ctx_get_buff_offset(conn) > 0);
}

static inline int conn_writeable(conn_t *conn) {
  return conn_ctx_check_ev(conn, ECONN_WRITEABLE) &
         (conn_ctx_get_buff_offset(conn) > 0);
}

static inline void conn_clear(conn_t *conn){
  memset(conn, 0, sizeof(*conn) - offsetof(conn_t, buf));
}

#endif /* __CONN_CTX_CLT */

