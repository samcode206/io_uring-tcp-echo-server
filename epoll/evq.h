#ifndef __EVQ_CLNT
#define __EVQ_CLNT

#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>

#define MAX_EVENTS 1024


typedef struct {
  size_t ev_q_head;
  size_t ev_q_tail;
  uint64_t ev_q[MAX_EVENTS];
} evq_t;

static inline size_t evq_get_head(evq_t *q) {
  return q->ev_q_head & (MAX_EVENTS - 1);
}

static inline size_t evq_get_tail(evq_t *q) {
  return q->ev_q_tail & (MAX_EVENTS - 1);
}

static inline size_t evq_get_space(evq_t *q) {
  return MAX_EVENTS - 1 - ((q->ev_q_head - q->ev_q_tail) & (MAX_EVENTS - 1));
}

static inline void evq_move_head(evq_t *q, int d) {
  q->ev_q_head = q->ev_q_head + d;
}

static inline void evq_move_tail(evq_t *q, int d) {
  q->ev_q_tail = q->ev_q_tail + d;
}

static inline int evq_is_empty(evq_t *q) {
  if ((evq_get_tail(q) == evq_get_head(q))) {
    return 1;
  }
  return 0;
}

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

static inline uint64_t evq_peek_evqe(evq_t *q) {
  if (LIKELY(evq_get_head(q) != evq_get_tail(q))) {
    return q->ev_q[evq_get_tail(q)];
  }

  return 0;
}

static inline uint64_t evq_add_evqe(evq_t *q, uint64_t qe) {
  if (LIKELY(evq_get_space(q) > 0)) {
    q->ev_q[evq_get_head(q)] = qe;
    evq_move_head(q, 1);

#if DIAGNOSTIC
    assert(c->n_qe == 1);
#endif

    return qe;
  } else {
    return 0;
  }
}

static inline void evq_delete_evqe(evq_t *q) {
  size_t tail = evq_get_tail(q);

#if DIAGNOSTIC
  assert(s->ev_q[tail] != NULL);
  assert(s->ev_q[tail]->n_qe == 1);
#endif

  q->ev_q[tail] = 0;
  evq_move_tail(q, 1);
}

// takes event at tail and moves it to head
static void server_evq_readd_evqe(evq_t *q) {
  uint64_t qe = q->ev_q[evq_get_tail(q)];

  size_t tail = evq_get_tail(q);
  q->ev_q[tail] = 0;
  evq_move_tail(q, 1);

  q->ev_q[evq_get_head(q)] = qe;
  evq_move_head(q, 1);
}

#endif

