#include <stdbool.h>
#include <stdio.h>


#define E_READABLE      (1 << 0)   // 0001
#define E_WRITEABLE     (1 << 1)   // 0010
#define E_READ_WRITE    (E_READABLE | E_WRITEABLE)  // 0011
#define E_SHOULD_CLOSE  (1 << 2)   // 0100


void conn_set_event(int *events, int eventMask) {
    *events |= eventMask;
}


void conn_unset_event(int *events, int eventMask) {
    *events &= ~eventMask;
}


bool conn_check_event(int events, int eventMask) {
    return (events & eventMask) != 0;
}

int main() {
    int events = 0; // no events set initially

    // Set events
    conn_set_event(&events, E_READ_WRITE);
    conn_set_event(&events, E_SHOULD_CLOSE);

    // Check events
    if (conn_check_event(events, E_READABLE)) {
        printf("E_READABLE is on\n");
    }
    if (conn_check_event(events, E_WRITEABLE)) {
        printf("E_WRITEABLE is on\n");
    }
    if (conn_check_event(events, E_SHOULD_CLOSE)) {
        printf("E_SHOULD_CLOSE is on\n");
    }
    printf("events: %d\n", events);
    // Unset an event
    conn_unset_event(&events, E_WRITEABLE);
    
    
    // Check after unsetting
    if (!conn_check_event(events, E_WRITEABLE)) {
        printf("E_WRITEABLE is off\n");
    }

    return 0;
}