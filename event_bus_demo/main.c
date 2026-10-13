#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void on_execute(event_t *event, void *user_data)
{
    const char *app = event_get_header(event, "Application");
    const char *data = event_get_header(event, "Application-Data");

    printf("[listener:%s] seq=%llu priority=%s ts=%llu execute app=%s data=%s body=%s\n",
           (char *)user_data,
           (unsigned long long)event->sequence,
           event_priority_name(event->priority),
           (unsigned long long)event->timestamp_us,
           app ? app : "",
           data ? data : "",
           event->body ? event->body : "");

    if (app && strcmp(app, "hold") == 0) {
        sleep(1);
    }
}

static void on_bridge_only(event_t *event, void *user_data)
{
    char *serialized = event_serialize(event);

    printf("[listener:%s] filtered bridge event:\n%s\n",
           (char *)user_data,
           serialized ? serialized : "<serialize failed>");
    free(serialized);
}

static void on_execute_removed(event_t *event, void *user_data)
{
    printf("[listener:%s] should-not-run app=%s\n",
           (char *)user_data,
           event_get_header(event, "Application"));
}

static void on_custom_sofia(event_t *event, void *user_data)
{
    printf("[listener:%s] seq=%llu custom subclass=%s user=%s\n",
           (char *)user_data,
           (unsigned long long)event->sequence,
           event->subclass ? event->subclass : "",
           event_get_header(event, "User"));
}

static void on_all(event_t *event, void *user_data)
{
    printf("[listener:%s] seq=%llu event=%s subclass=%s\n",
           (char *)user_data,
           (unsigned long long)event->sequence,
           event_name(event->type),
           event->subclass ? event->subclass : "");
}

int main(void)
{
    event_bus_t bus;
    event_bus_stats_t stats;
    event_t *event;
    event_listener_t *removed_listener = NULL;
    event_filter_rule_t bridge_filter[] = {
        { "Application", "bridge" }
    };

    if (event_bus_init_ex(&bus, 1024, 1, EVENT_QUEUE_BLOCK) != 0) {
        fprintf(stderr, "event bus init failed\n");
        return 1;
    }

    event_reserve_subclass(&bus, "mod_sofia_like", "sofia::register");

    event_bind(&bus, EVT_CHANNEL_EXECUTE, NULL, on_execute, "execute");
    event_bind_filtered(&bus, EVT_CHANNEL_EXECUTE, NULL, bridge_filter, 1, on_bridge_only, "bridge-filter", NULL);
    event_bind_removable(&bus, EVT_CHANNEL_EXECUTE, NULL, on_execute_removed, "removed", &removed_listener);
    event_unbind(&bus, &removed_listener);
    event_bind(&bus, EVT_CUSTOM, "sofia::register", on_custom_sofia, "custom-sofia");
    event_bind(&bus, EVT_ALL, NULL, on_all, "all");

    event = event_create_here(EVT_CHANNEL_EXECUTE, NULL);
    event_add_header(event, "Application", "hold");
    event_add_header(event, "Application-Data", "let-queue-build");
    event_fire(&bus, event);

    event = event_create(EVT_CHANNEL_EXECUTE, NULL);
    event_set_priority(event, EVENT_PRIORITY_LOW);
    event_add_header(event, "Application", "playback");
    event_add_header(event, "Application-Data", "ivr/ivr-welcome.wav");
    event_fire(&bus, event);

    event = event_create_here(EVT_CHANNEL_EXECUTE, NULL);
    event_set_priority(event, EVENT_PRIORITY_HIGH);
    event_add_header(event, "Application", "bridge");
    event_add_header(event, "Application-Data", "user/1001");
    event_set_body(event, "demo body");
    event_fire(&bus, event);

    event = event_create_here(EVT_CUSTOM, "sofia::register");
    event_add_header(event, "User", "1001");
    event_add_header(event, "Domain", "example.com");
    event_fire(&bus, event);

    do {
        sleep(1);
        event_bus_get_stats(&bus, &stats);
    } while (stats.delivered + stats.dropped < 4u);
    printf("[stats] fired=%llu delivered=%llu dropped=%llu queued=%d high=%d normal=%d low=%d peak=%d workers=%d\n",
           (unsigned long long)stats.fired,
           (unsigned long long)stats.delivered,
           (unsigned long long)stats.dropped,
           stats.queued,
           stats.queued_high,
           stats.queued_normal,
           stats.queued_low,
           stats.queue_peak,
           stats.worker_count);
    event_bus_shutdown(&bus);

    return 0;
}
