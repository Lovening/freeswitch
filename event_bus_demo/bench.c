#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static void on_event(event_t *event, void *user_data)
{
    (void)event;
    (void)user_data;
}

static uint64_t wallclock_us(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000000u) + (uint64_t)tv.tv_usec;
}

int main(int argc, char **argv)
{
    event_bus_t bus;
    event_bus_stats_t stats;
    uint64_t start_us;
    uint64_t end_us;
    int total = 100000;
    int i;

    if (argc > 1) {
        total = atoi(argv[1]);
        if (total <= 0) {
            total = 100000;
        }
    }

    if (event_bus_init_ex(&bus, 8192, 4, EVENT_QUEUE_BLOCK) != 0) {
        fprintf(stderr, "event bus init failed\n");
        return 1;
    }

    event_bind(&bus, EVT_CHANNEL_EXECUTE, NULL, on_event, NULL);

    start_us = wallclock_us();

    for (i = 0; i < total; i++) {
        event_t *event = event_create(EVT_CHANNEL_EXECUTE, NULL);
        event_add_header(event, "Application", "bridge");
        event_add_header(event, "Application-Data", "user/1001");
        event_fire(&bus, event);
    }

    for (;;) {
        event_bus_get_stats(&bus, &stats);
        if (stats.delivered + stats.dropped >= (uint64_t)total) {
            break;
        }
    }

    end_us = wallclock_us();
    event_bus_get_stats(&bus, &stats);

    printf("events=%d fired=%llu delivered=%llu dropped=%llu peak=%d elapsed_us=%llu rate=%.2f events/sec\n",
           total,
           (unsigned long long)stats.fired,
           (unsigned long long)stats.delivered,
           (unsigned long long)stats.dropped,
           stats.queue_peak,
           (unsigned long long)(end_us - start_us),
           (double)total * 1000000.0 / (double)(end_us - start_us));

    event_bus_shutdown(&bus);
    return 0;
}
