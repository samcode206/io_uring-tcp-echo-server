#include <stdint.h>
#include <stdio.h>

#define FD_UNUSED -1
#define FD_CLOSING -2
#define FD_OPEN 1

#define FD_MASK ((1ULL << 21) - 1)            // 21 bits
#define SRV_LIM_MAX_FD 2097151

#define BGID_MASK (((1ULL << 17) - 1) << 21) // 17 bits shifted by 21
#define SRV_LIM_MAX_BGS 131071

#define EVENT_MASK (3ULL << 38)               // 2 bits shifted by 38

#define FD_SHIFT 0
#define BGID_SHIFT 21
#define EVENT_SHIFT 38



#define EV_ACCEPT 0
#define EV_RECV 1
#define EV_SEND 2
#define EV_CLOSE 3

static inline void set_fd(uint64_t *data, uint32_t fd) {
  *data = (*data & ~FD_MASK) | ((uint64_t)fd << FD_SHIFT);
}

static inline void set_bgid(uint64_t *data, uint32_t index) {
  *data = (*data & ~BGID_MASK) | ((uint64_t)index << BGID_SHIFT);
}

static inline void set_event(uint64_t *data, uint8_t event) {
  *data = (*data & ~EVENT_MASK) | ((uint64_t)event << EVENT_SHIFT);
}

static inline uint32_t get_fd(uint64_t data) {
  return (data & FD_MASK) >> FD_SHIFT;
}
static inline uint32_t get_bgid(uint64_t data) {
  return (data & BGID_MASK) >> BGID_SHIFT;
}
static inline uint8_t get_event(uint64_t data) {
  return (data & EVENT_MASK) >> EVENT_SHIFT;
}

int main() {
  uint64_t data = 0;

  printf("%d %d %d\n", get_fd(data), get_bgid(data), get_event(data));

  // Test
  set_fd(&data, 1);    // Maximum FD value with 21 bits
  set_bgid(&data, 2);  // Some index value
  set_event(&data, 1); // Some event value

  printf("File Descriptor: %u\n", get_fd(data));
  set_fd(&data, 91);
  printf("File Descriptor: %u\n", get_fd(data));

  printf("Index: %u\n", get_bgid(data));
  set_bgid(&data, 41);
  printf("Index: %u\n", get_bgid(data));

  printf("Event: %u\n", get_event(data));
  set_event(&data, 3);
  printf("Event: %u\n", get_event(data));

  return 0;
}
