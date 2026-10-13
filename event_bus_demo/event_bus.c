#include "event_bus.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef struct callback_entry {
    event_callback_t callback;
    void *user_data;
} callback_entry_t;

static const char *EVENT_NAMES[] = {
    "CUSTOM",
    "CHANNEL_CREATE",
    "CHANNEL_EXECUTE",
    "CHANNEL_HANGUP",
    "ALL"
};

static const char *PRIORITY_NAMES[] = {
    "NORMAL",
    "LOW",
    "HIGH"
};

static int valid_priority(event_priority_t priority)
{
    return priority >= EVENT_PRIORITY_NORMAL && priority < EVENT_PRIORITY_COUNT;
}

static char *xstrdup(const char *s)
{
    char *p;

    if (!s) {
        return NULL;
    }

    p = malloc(strlen(s) + 1);
    if (p) {
        strcpy(p, s);
    }

    return p;
}

static int subclass_match(const char *listener_subclass, const char *event_subclass)
{
    if (!listener_subclass) {
        return 1;
    }

    if (!event_subclass) {
        return 0;
    }

    return strcmp(listener_subclass, event_subclass) == 0;
}

static int filters_match(event_t *event, event_filter_t *filters)
{
    event_filter_t *filter;

    for (filter = filters; filter; filter = filter->next) {
        const char *value = event_get_header(event, filter->name);

        if (!value || strcmp(value, filter->value) != 0) {
            return 0;
        }
    }

    return 1;
}

static uint64_t now_us(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000000u) + (uint64_t)tv.tv_usec;
}

static event_header_t *find_header(event_t *event, const char *name)
{
    event_header_t *header;

    if (!event || !name) {
        return NULL;
    }

    for (header = event->headers; header; header = header->next) {
        if (strcmp(header->name, name) == 0) {
            return header;
        }
    }

    return NULL;
}

static void event_set_header(event_t *event, const char *name, const char *value)
{
    event_header_t *header;

    if (!event || !name || !value) {
        return;
    }

    header = find_header(event, name);
    if (header) {
        char *copy = xstrdup(value);
        if (!copy) {
            return;
        }
        free(header->value);
        header->value = copy;
        return;
    }

    event_add_header(event, name, value);
}

static void event_set_headerf(event_t *event, const char *name, const char *fmt, ...)
{
    va_list ap;
    va_list aq;
    int needed;
    char *value;

    va_start(ap, fmt);
    va_copy(aq, ap);
    needed = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);

    if (needed < 0) {
        va_end(ap);
        return;
    }

    value = malloc((size_t)needed + 1u);
    if (!value) {
        va_end(ap);
        return;
    }

    vsnprintf(value, (size_t)needed + 1u, fmt, ap);
    va_end(ap);
    event_set_header(event, name, value);
    free(value);
}

static void event_prepare_for_delivery(event_t *event)
{
    event_set_header(event, "Event-Name", event_name(event->type));
    event_set_headerf(event, "Event-Sequence", "%llu", (unsigned long long)event->sequence);
    event_set_headerf(event, "Event-Date-Timestamp", "%llu", (unsigned long long)event->timestamp_us);
    event_set_header(event, "Event-Priority", event_priority_name(event->priority));

    if (event->subclass) {
        event_set_header(event, "Event-Subclass", event->subclass);
    }

    if (event->source_file) {
        event_set_header(event, "Event-Calling-File", event->source_file);
    }

    if (event->source_func) {
        event_set_header(event, "Event-Calling-Function", event->source_func);
    }

    if (event->source_line > 0) {
        event_set_headerf(event, "Event-Calling-Line-Number", "%d", event->source_line);
    }
}

static int appendf(char **buffer, size_t *used, size_t *capacity, const char *fmt, ...)
{
    va_list ap;
    va_list aq;
    int needed;
    char *tmp;

    va_start(ap, fmt);
    va_copy(aq, ap);
    needed = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);

    if (needed < 0) {
        va_end(ap);
        return -1;
    }

    if (*used + (size_t)needed + 1u > *capacity) {
        size_t new_capacity = *capacity ? *capacity : 256u;

        while (*used + (size_t)needed + 1u > new_capacity) {
            new_capacity *= 2u;
        }

        tmp = realloc(*buffer, new_capacity);
        if (!tmp) {
            va_end(ap);
            return -1;
        }

        *buffer = tmp;
        *capacity = new_capacity;
    }

    vsnprintf(*buffer + *used, *capacity - *used, fmt, ap);
    va_end(ap);
    *used += (size_t)needed;

    return 0;
}

static int add_callback(callback_entry_t **callbacks,
                        size_t *count,
                        size_t *capacity,
                        event_callback_t callback,
                        void *user_data)
{
    callback_entry_t *tmp;

    if (*count == *capacity) {
        size_t new_capacity = *capacity ? *capacity * 2u : 8u;

        tmp = realloc(*callbacks, new_capacity * sizeof(**callbacks));
        if (!tmp) {
            return -1;
        }

        *callbacks = tmp;
        *capacity = new_capacity;
    }

    (*callbacks)[*count].callback = callback;
    (*callbacks)[*count].user_data = user_data;
    (*count)++;

    return 0;
}

static int queue_alloc(event_queue_t *queue, int queue_size)
{
    queue->events = calloc((size_t)queue_size, sizeof(event_t *));
    if (!queue->events) {
        return -1;
    }

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    return 0;
}

static void queue_free(event_queue_t *queue)
{
    free(queue->events);
    queue->events = NULL;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
}

static int queue_push(event_queue_t *queue, int queue_size, event_t *event)
{
    if (queue->count == queue_size) {
        return -1;
    }

    queue->events[queue->tail] = event;
    queue->tail = (queue->tail + 1) % queue_size;
    queue->count++;
    return 0;
}

static event_t *queue_pop(event_queue_t *queue, int queue_size)
{
    event_t *event;

    if (queue->count == 0) {
        return NULL;
    }

    event = queue->events[queue->head];
    queue->events[queue->head] = NULL;
    queue->head = (queue->head + 1) % queue_size;
    queue->count--;
    return event;
}

static event_queue_t *event_bus_queue_for_priority(event_bus_t *bus, event_priority_t priority)
{
    if (!valid_priority(priority)) {
        priority = EVENT_PRIORITY_NORMAL;
    }

    return &bus->queues[priority];
}

static event_queue_t *event_bus_queue_for_event(event_bus_t *bus, event_t *event)
{
    return event_bus_queue_for_priority(bus, event->priority);
}

static event_t *event_bus_pop_next_locked(event_bus_t *bus)
{
    static const event_priority_t order[] = {
        EVENT_PRIORITY_HIGH,
        EVENT_PRIORITY_NORMAL,
        EVENT_PRIORITY_LOW
    };
    size_t i;

    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        event_queue_t *queue = &bus->queues[order[i]];

        if (queue->count > 0) {
            return queue_pop(queue, bus->queue_size);
        }
    }

    return NULL;
}

static int event_bus_alloc_queues(event_bus_t *bus)
{
    int i;

    for (i = 0; i < EVENT_PRIORITY_COUNT; i++) {
        if (queue_alloc(&bus->queues[i], bus->queue_size) != 0) {
            while (--i >= 0) {
                queue_free(&bus->queues[i]);
            }
            return -1;
        }
    }

    return 0;
}

static void event_bus_free_queues(event_bus_t *bus)
{
    int i;

    for (i = 0; i < EVENT_PRIORITY_COUNT; i++) {
        queue_free(&bus->queues[i]);
    }
}

static event_filter_t *copy_filters(const event_filter_rule_t *filters, size_t filter_count)
{
    event_filter_t *head = NULL;
    event_filter_t *tail = NULL;
    size_t i;

    for (i = 0; i < filter_count; i++) {
        event_filter_t *filter;

        if (!filters[i].name || !filters[i].value) {
            continue;
        }

        filter = calloc(1, sizeof(*filter));
        if (!filter) {
            break;
        }

        filter->name = xstrdup(filters[i].name);
        filter->value = xstrdup(filters[i].value);
        if (!filter->name || !filter->value) {
            free(filter->name);
            free(filter->value);
            free(filter);
            break;
        }

        if (tail) {
            tail->next = filter;
        } else {
            head = filter;
        }
        tail = filter;
    }

    return head;
}

static void free_filters(event_filter_t *filter)
{
    while (filter) {
        event_filter_t *next = filter->next;
        free(filter->name);
        free(filter->value);
        free(filter);
        filter = next;
    }
}

static void free_listener(event_listener_t *listener)
{
    if (!listener) {
        return;
    }

    free(listener->subclass);
    free_filters(listener->filters);
    free(listener);
}

static void snapshot_listeners(event_bus_t *bus,
                               event_t *event,
                               callback_entry_t **callbacks,
                               size_t *count,
                               size_t *capacity,
                               event_type_t type)
{
    event_listener_t *listener;

    for (listener = bus->listeners[type]; listener; listener = listener->next) {
        if (subclass_match(listener->subclass, event->subclass) && filters_match(event, listener->filters)) {
            if (add_callback(callbacks, count, capacity, listener->callback, listener->user_data) != 0) {
                break;
            }
        }
    }
}

static void event_deliver(event_bus_t *bus, event_t *event)
{
    callback_entry_t *callbacks = NULL;
    size_t callback_count = 0;
    size_t callback_capacity = 0;
    size_t i;

    pthread_mutex_lock(&bus->listener_mutex);

    snapshot_listeners(bus, event, &callbacks, &callback_count, &callback_capacity, event->type);

    if (event->type != EVT_ALL) {
        snapshot_listeners(bus, event, &callbacks, &callback_count, &callback_capacity, EVT_ALL);
    }

    pthread_mutex_unlock(&bus->listener_mutex);

    for (i = 0; i < callback_count; i++) {
        callbacks[i].callback(event, callbacks[i].user_data);
    }

    free(callbacks);

    pthread_mutex_lock(&bus->queue_mutex);
    bus->delivered++;
    pthread_mutex_unlock(&bus->queue_mutex);
}

static void *event_worker(void *arg)
{
    event_bus_t *bus = arg;

    for (;;) {
        event_t *event;

        pthread_mutex_lock(&bus->queue_mutex);

        while (bus->count == 0 && bus->running) {
            pthread_cond_wait(&bus->not_empty, &bus->queue_mutex);
        }

        if (!bus->running && bus->count == 0) {
            pthread_mutex_unlock(&bus->queue_mutex);
            break;
        }

        event = event_bus_pop_next_locked(bus);
        if (event) {
            bus->count--;
            pthread_cond_signal(&bus->not_full);
        }

        pthread_mutex_unlock(&bus->queue_mutex);

        if (!event) {
            continue;
        }

        event_deliver(bus, event);
        event_destroy(event);
    }

    return NULL;
}

int event_bus_init_ex(event_bus_t *bus,
                      int queue_size,
                      int worker_count,
                      event_queue_policy_t queue_policy)
{
    int i;

    memset(bus, 0, sizeof(*bus));

    bus->queue_size = queue_size > 0 ? queue_size : 1024;
    bus->worker_count = worker_count > 0 ? worker_count : 1;
    bus->queue_policy = queue_policy;

    if (event_bus_alloc_queues(bus) != 0) {
        memset(bus, 0, sizeof(*bus));
        return -1;
    }

    bus->workers = calloc((size_t)bus->worker_count, sizeof(pthread_t));
    if (!bus->workers) {
        event_bus_free_queues(bus);
        memset(bus, 0, sizeof(*bus));
        return -1;
    }

    pthread_mutex_init(&bus->listener_mutex, NULL);
    pthread_mutex_init(&bus->subclass_mutex, NULL);
    pthread_mutex_init(&bus->queue_mutex, NULL);
    pthread_cond_init(&bus->not_empty, NULL);
    pthread_cond_init(&bus->not_full, NULL);

    bus->running = 1;

    for (i = 0; i < bus->worker_count; i++) {
        if (pthread_create(&bus->workers[i], NULL, event_worker, bus) != 0) {
            pthread_mutex_lock(&bus->queue_mutex);
            bus->running = 0;
            pthread_cond_broadcast(&bus->not_empty);
            pthread_cond_broadcast(&bus->not_full);
            pthread_mutex_unlock(&bus->queue_mutex);

            while (--i >= 0) {
                pthread_join(bus->workers[i], NULL);
            }

            pthread_cond_destroy(&bus->not_full);
            pthread_cond_destroy(&bus->not_empty);
            pthread_mutex_destroy(&bus->queue_mutex);
            pthread_mutex_destroy(&bus->subclass_mutex);
            pthread_mutex_destroy(&bus->listener_mutex);
            free(bus->workers);
            event_bus_free_queues(bus);
            memset(bus, 0, sizeof(*bus));
            return -1;
        }
    }

    return 0;
}

int event_bus_init(event_bus_t *bus, int queue_size)
{
    return event_bus_init_ex(bus, queue_size, 1, EVENT_QUEUE_BLOCK);
}

void event_bus_shutdown(event_bus_t *bus)
{
    int i;

    if (!bus || !bus->queues[EVENT_PRIORITY_NORMAL].events) {
        return;
    }

    pthread_mutex_lock(&bus->queue_mutex);
    bus->running = 0;
    pthread_cond_broadcast(&bus->not_empty);
    pthread_cond_broadcast(&bus->not_full);
    pthread_mutex_unlock(&bus->queue_mutex);

    for (i = 0; i < bus->worker_count; i++) {
        pthread_join(bus->workers[i], NULL);
    }

    while (bus->count > 0) {
        event_t *event = event_bus_pop_next_locked(bus);

        if (!event) {
            break;
        }

        bus->count--;
        event_destroy(event);
    }

    for (i = 0; i <= EVT_ALL; i++) {
        event_listener_t *listener = bus->listeners[i];

        while (listener) {
            event_listener_t *next = listener->next;
            free_listener(listener);
            listener = next;
        }
    }

    while (bus->subclasses) {
        event_subclass_t *next = bus->subclasses->next;
        free(bus->subclasses->owner);
        free(bus->subclasses->name);
        free(bus->subclasses);
        bus->subclasses = next;
    }

    free(bus->workers);
    event_bus_free_queues(bus);

    pthread_cond_destroy(&bus->not_full);
    pthread_cond_destroy(&bus->not_empty);
    pthread_mutex_destroy(&bus->queue_mutex);
    pthread_mutex_destroy(&bus->subclass_mutex);
    pthread_mutex_destroy(&bus->listener_mutex);

    memset(bus, 0, sizeof(*bus));
}

void event_bus_get_stats(event_bus_t *bus, event_bus_stats_t *stats)
{
    if (!bus || !stats) {
        return;
    }

    pthread_mutex_lock(&bus->queue_mutex);
    stats->fired = bus->fired;
    stats->dropped = bus->dropped;
    stats->delivered = bus->delivered;
    stats->queued = bus->count;
    stats->queued_high = bus->queues[EVENT_PRIORITY_HIGH].count;
    stats->queued_normal = bus->queues[EVENT_PRIORITY_NORMAL].count;
    stats->queued_low = bus->queues[EVENT_PRIORITY_LOW].count;
    stats->queue_peak = bus->queue_peak;
    stats->worker_count = bus->worker_count;
    pthread_mutex_unlock(&bus->queue_mutex);
}

event_t *event_create_detailed(event_type_t type,
                               const char *subclass,
                               const char *file,
                               const char *func,
                               int line)
{
    event_t *event;

    if (type > EVT_ALL) {
        return NULL;
    }

    event = calloc(1, sizeof(*event));
    if (!event) {
        return NULL;
    }

    event->type = type;
    event->priority = EVENT_PRIORITY_NORMAL;
    event->subclass = xstrdup(subclass);
    event->source_file = xstrdup(file);
    event->source_func = xstrdup(func);
    event->source_line = line;

    return event;
}

event_t *event_create(event_type_t type, const char *subclass)
{
    return event_create_detailed(type, subclass, NULL, NULL, 0);
}

const char *event_name(event_type_t type)
{
    if (type > EVT_ALL) {
        return "UNKNOWN";
    }

    return EVENT_NAMES[type];
}

const char *event_priority_name(event_priority_t priority)
{
    if (!valid_priority(priority)) {
        return "UNKNOWN";
    }

    return PRIORITY_NAMES[priority];
}

void event_set_priority(event_t *event, event_priority_t priority)
{
    if (!event || !valid_priority(priority)) {
        return;
    }

    event->priority = priority;
}

void event_add_header(event_t *event, const char *name, const char *value)
{
    event_header_t *header;

    if (!event || !name || !value) {
        return;
    }

    header = calloc(1, sizeof(*header));
    if (!header) {
        return;
    }

    header->name = xstrdup(name);
    header->value = xstrdup(value);

    if (!header->name || !header->value) {
        free(header->name);
        free(header->value);
        free(header);
        return;
    }

    if (event->last_header) {
        event->last_header->next = header;
    } else {
        event->headers = header;
    }

    event->last_header = header;
}

void event_set_body(event_t *event, const char *body)
{
    if (!event) {
        return;
    }

    free(event->body);
    event->body = xstrdup(body);
}

const char *event_get_header(event_t *event, const char *name)
{
    event_header_t *header;

    if (!event || !name) {
        return NULL;
    }

    for (header = event->headers; header; header = header->next) {
        if (strcmp(header->name, name) == 0) {
            return header->value;
        }
    }

    return NULL;
}

char *event_serialize(event_t *event)
{
    event_header_t *header;
    char *buffer = NULL;
    size_t used = 0;
    size_t capacity = 0;

    if (!event) {
        return NULL;
    }

    for (header = event->headers; header; header = header->next) {
        if (appendf(&buffer, &used, &capacity, "%s: %s\n", header->name, header->value) != 0) {
            free(buffer);
            return NULL;
        }
    }

    if (event->body) {
        if (appendf(&buffer, &used, &capacity, "\n%s", event->body) != 0) {
            free(buffer);
            return NULL;
        }
    }

    return buffer;
}

void event_destroy(event_t *event)
{
    event_header_t *header;

    if (!event) {
        return;
    }

    header = event->headers;
    while (header) {
        event_header_t *next = header->next;
        free(header->name);
        free(header->value);
        free(header);
        header = next;
    }

    free(event->subclass);
    free(event->source_file);
    free(event->source_func);
    free(event->body);
    free(event);
}

int event_reserve_subclass(event_bus_t *bus, const char *owner, const char *subclass)
{
    event_subclass_t *node;

    if (!bus || !owner || !subclass) {
        return -1;
    }

    pthread_mutex_lock(&bus->subclass_mutex);

    for (node = bus->subclasses; node; node = node->next) {
        if (strcmp(node->name, subclass) == 0) {
            pthread_mutex_unlock(&bus->subclass_mutex);
            return strcmp(node->owner, owner) == 0 ? 0 : 1;
        }
    }

    node = calloc(1, sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&bus->subclass_mutex);
        return -1;
    }

    node->owner = xstrdup(owner);
    node->name = xstrdup(subclass);
    if (!node->owner || !node->name) {
        free(node->owner);
        free(node->name);
        free(node);
        pthread_mutex_unlock(&bus->subclass_mutex);
        return -1;
    }

    node->next = bus->subclasses;
    bus->subclasses = node;

    pthread_mutex_unlock(&bus->subclass_mutex);
    return 0;
}

int event_free_subclass(event_bus_t *bus, const char *owner, const char *subclass)
{
    event_subclass_t **link;

    if (!bus || !owner || !subclass) {
        return -1;
    }

    pthread_mutex_lock(&bus->subclass_mutex);

    for (link = &bus->subclasses; *link; link = &(*link)->next) {
        event_subclass_t *node = *link;

        if (strcmp(node->name, subclass) == 0) {
            if (strcmp(node->owner, owner) != 0) {
                pthread_mutex_unlock(&bus->subclass_mutex);
                return 1;
            }

            *link = node->next;
            pthread_mutex_unlock(&bus->subclass_mutex);
            free(node->owner);
            free(node->name);
            free(node);
            return 0;
        }
    }

    pthread_mutex_unlock(&bus->subclass_mutex);
    return -1;
}

int event_subclass_reserved(event_bus_t *bus, const char *subclass)
{
    event_subclass_t *node;

    if (!bus || !subclass) {
        return 0;
    }

    pthread_mutex_lock(&bus->subclass_mutex);

    for (node = bus->subclasses; node; node = node->next) {
        if (strcmp(node->name, subclass) == 0) {
            pthread_mutex_unlock(&bus->subclass_mutex);
            return 1;
        }
    }

    pthread_mutex_unlock(&bus->subclass_mutex);
    return 0;
}

int event_bind_filtered(event_bus_t *bus,
                        event_type_t type,
                        const char *subclass,
                        const event_filter_rule_t *filters,
                        size_t filter_count,
                        event_callback_t callback,
                        void *user_data,
                        event_listener_t **listener_out)
{
    event_listener_t *listener;

    if (listener_out) {
        *listener_out = NULL;
    }

    if (!bus || !callback || type > EVT_ALL) {
        return -1;
    }

    listener = calloc(1, sizeof(*listener));
    if (!listener) {
        return -1;
    }

    listener->type = type;
    listener->subclass = xstrdup(subclass);
    listener->filters = copy_filters(filters, filter_count);
    listener->callback = callback;
    listener->user_data = user_data;

    pthread_mutex_lock(&bus->listener_mutex);
    listener->next = bus->listeners[type];
    bus->listeners[type] = listener;
    pthread_mutex_unlock(&bus->listener_mutex);

    if (listener_out) {
        *listener_out = listener;
    }

    return 0;
}

int event_bind_removable(event_bus_t *bus,
                         event_type_t type,
                         const char *subclass,
                         event_callback_t callback,
                         void *user_data,
                         event_listener_t **listener_out)
{
    return event_bind_filtered(bus, type, subclass, NULL, 0, callback, user_data, listener_out);
}

int event_bind(event_bus_t *bus,
               event_type_t type,
               const char *subclass,
               event_callback_t callback,
               void *user_data)
{
    return event_bind_removable(bus, type, subclass, callback, user_data, NULL);
}

int event_unbind(event_bus_t *bus, event_listener_t **listener)
{
    event_listener_t **link;
    event_listener_t *target;

    if (!bus || !listener || !*listener || (*listener)->type > EVT_ALL) {
        return -1;
    }

    target = *listener;

    pthread_mutex_lock(&bus->listener_mutex);

    for (link = &bus->listeners[target->type]; *link; link = &(*link)->next) {
        if (*link == target) {
            *link = target->next;
            pthread_mutex_unlock(&bus->listener_mutex);

            free_listener(target);
            *listener = NULL;
            return 0;
        }
    }

    pthread_mutex_unlock(&bus->listener_mutex);
    return -1;
}

int event_fire(event_bus_t *bus, event_t *event)
{
    event_queue_t *queue;

    if (!bus || !event) {
        event_destroy(event);
        return -1;
    }

    if (event->type == EVT_CUSTOM && event->subclass && !event_subclass_reserved(bus, event->subclass)) {
        event_destroy(event);
        return -1;
    }

    if (!valid_priority(event->priority)) {
        event->priority = EVENT_PRIORITY_NORMAL;
    }

    pthread_mutex_lock(&bus->queue_mutex);

    queue = event_bus_queue_for_event(bus, event);
    event->sequence = ++bus->next_sequence;
    event->timestamp_us = now_us();
    event_prepare_for_delivery(event);

    while (queue->count == bus->queue_size && bus->running) {
        if (bus->queue_policy == EVENT_QUEUE_DROP_NEWEST) {
            bus->dropped++;
            pthread_mutex_unlock(&bus->queue_mutex);
            event_destroy(event);
            return 1;
        }

        pthread_cond_wait(&bus->not_full, &bus->queue_mutex);
    }

    if (!bus->running) {
        pthread_mutex_unlock(&bus->queue_mutex);
        event_destroy(event);
        return -1;
    }

    if (queue_push(queue, bus->queue_size, event) != 0) {
        bus->dropped++;
        pthread_mutex_unlock(&bus->queue_mutex);
        event_destroy(event);
        return 1;
    }

    bus->count++;
    bus->fired++;

    if (bus->count > bus->queue_peak) {
        bus->queue_peak = bus->count;
    }

    pthread_cond_signal(&bus->not_empty);
    pthread_mutex_unlock(&bus->queue_mutex);

    return 0;
}
