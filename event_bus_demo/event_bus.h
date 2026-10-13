#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    EVT_CUSTOM = 0,
    EVT_CHANNEL_CREATE,
    EVT_CHANNEL_EXECUTE,
    EVT_CHANNEL_HANGUP,
    EVT_ALL
} event_type_t;

typedef enum {
    EVENT_PRIORITY_NORMAL = 0,
    EVENT_PRIORITY_LOW,
    EVENT_PRIORITY_HIGH,
    EVENT_PRIORITY_COUNT
} event_priority_t;

typedef enum {
    EVENT_QUEUE_BLOCK = 0,
    EVENT_QUEUE_DROP_NEWEST
} event_queue_policy_t;

typedef struct event event_t;

typedef struct event_queue {
    event_t **events;
    int head;
    int tail;
    int count;
} event_queue_t;

typedef struct event_header {
    char *name;
    char *value;
    struct event_header *next;
} event_header_t;

struct event {
    event_type_t type;
    event_priority_t priority;
    uint64_t sequence;
    uint64_t timestamp_us;
    char *subclass;
    char *source_file;
    char *source_func;
    int source_line;
    event_header_t *headers;
    event_header_t *last_header;
    char *body;
};

typedef struct event_filter_rule {
    const char *name;
    const char *value;
} event_filter_rule_t;

typedef struct event_filter {
    char *name;
    char *value;
    struct event_filter *next;
} event_filter_t;

typedef void (*event_callback_t)(event_t *event, void *user_data);

typedef struct event_listener {
    event_type_t type;
    char *subclass;
    event_filter_t *filters;
    event_callback_t callback;
    void *user_data;
    struct event_listener *next;
} event_listener_t;

typedef struct event_subclass {
    char *owner;
    char *name;
    struct event_subclass *next;
} event_subclass_t;

typedef struct event_bus_stats {
    uint64_t fired;
    uint64_t dropped;
    uint64_t delivered;
    int queued;
    int queued_high;
    int queued_normal;
    int queued_low;
    int queue_peak;
    int worker_count;
} event_bus_stats_t;

typedef struct event_bus {
    event_listener_t *listeners[EVT_ALL + 1];
    event_subclass_t *subclasses;

    pthread_mutex_t listener_mutex;
    pthread_mutex_t subclass_mutex;

    event_queue_t queues[EVENT_PRIORITY_COUNT];
    int queue_size;
    int count;
    int queue_peak;

    pthread_mutex_t queue_mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    pthread_t *workers;
    int worker_count;
    int running;

    uint64_t next_sequence;
    uint64_t fired;
    uint64_t dropped;
    uint64_t delivered;

    event_queue_policy_t queue_policy;
} event_bus_t;

int event_bus_init(event_bus_t *bus, int queue_size);
int event_bus_init_ex(event_bus_t *bus,
                      int queue_size,
                      int worker_count,
                      event_queue_policy_t queue_policy);
void event_bus_shutdown(event_bus_t *bus);
void event_bus_get_stats(event_bus_t *bus, event_bus_stats_t *stats);

event_t *event_create(event_type_t type, const char *subclass);
event_t *event_create_detailed(event_type_t type,
                               const char *subclass,
                               const char *file,
                               const char *func,
                               int line);
#define event_create_here(_type, _subclass) \
    event_create_detailed((_type), (_subclass), __FILE__, __func__, __LINE__)

const char *event_name(event_type_t type);
const char *event_priority_name(event_priority_t priority);
void event_set_priority(event_t *event, event_priority_t priority);
void event_add_header(event_t *event, const char *name, const char *value);
void event_set_body(event_t *event, const char *body);
const char *event_get_header(event_t *event, const char *name);
char *event_serialize(event_t *event);
void event_destroy(event_t *event);

int event_reserve_subclass(event_bus_t *bus, const char *owner, const char *subclass);
int event_free_subclass(event_bus_t *bus, const char *owner, const char *subclass);
int event_subclass_reserved(event_bus_t *bus, const char *subclass);

int event_bind(event_bus_t *bus,
               event_type_t type,
               const char *subclass,
               event_callback_t callback,
               void *user_data);

int event_bind_removable(event_bus_t *bus,
                         event_type_t type,
                         const char *subclass,
                         event_callback_t callback,
                         void *user_data,
                         event_listener_t **listener_out);

int event_bind_filtered(event_bus_t *bus,
                        event_type_t type,
                        const char *subclass,
                        const event_filter_rule_t *filters,
                        size_t filter_count,
                        event_callback_t callback,
                        void *user_data,
                        event_listener_t **listener_out);

int event_unbind(event_bus_t *bus, event_listener_t **listener);

int event_fire(event_bus_t *bus, event_t *event);

#endif
