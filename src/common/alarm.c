/* SPDX-License-Identifier: MIT */
#include "openbios/alarm.h"

static int active(const struct ob_alarm_event *event) {
    return event->id > 0;
}

static int before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static int expired(const struct ob_alarm_queue *q,
                   const struct ob_alarm_event *event) {
    return (int32_t)(q->now - event->deadline) >= 0;
}

void ob_alarm_init(struct ob_alarm_queue *q) {
    if (!q) return;
    ob_zero(q, sizeof(*q));
}

static int id_in_use(const struct ob_alarm_queue *q, uint32_t id) {
    int i;
    for (i = 0; i < OB_MAX_ALARMS; ++i)
        if (active(&q->slots[i]) && (uint32_t)q->slots[i].id == id) return 1;
    return 0;
}

int ob_alarm_set(struct ob_alarm_queue *q, uint16_t time, uint32_t callback,
                 uint32_t common, uint32_t gp) {
    int slot = -1;
    int32_t id;
    uint32_t candidate;
    int i, tries;
    if (!q || !callback) return OB_EINVAL;
    for (i = 0; i < OB_MAX_ALARMS; ++i) {
        if (!active(&q->slots[i])) { slot = i; break; }
    }
    if (slot < 0) return OB_ENOSPC;

    candidate = q->next_id;
    for (tries = 0; tries <= OB_MAX_ALARMS; ++tries) {
        if (candidate >= (uint32_t)INT32_MAX) candidate = 1;
        else ++candidate;
        if (!id_in_use(q, candidate)) break;
    }
    if (tries > OB_MAX_ALARMS) return OB_ENOSPC;
    id = (int32_t)candidate;
    q->next_id = candidate;

    q->slots[slot].id = id;
    q->slots[slot].callback = callback;
    q->slots[slot].common = common;
    q->slots[slot].gp = gp;
    q->slots[slot].time = time;
    q->slots[slot].deadline = q->now + (time ? (uint32_t)time : 1u);
    q->slots[slot].order = q->sequence++;
    return id;
}

int ob_alarm_release(struct ob_alarm_queue *q, int id) {
    int i;
    if (!q || id <= 0) return OB_EINVAL;
    for (i = 0; i < OB_MAX_ALARMS; ++i) {
        if (q->slots[i].id != id) continue;
        ob_zero(&q->slots[i], sizeof(q->slots[i]));
        return OB_OK;
    }
    return OB_EINVAL;
}

void ob_alarm_advance(struct ob_alarm_queue *q, uint32_t elapsed) {
    if (!q) return;
    q->now += elapsed;
}

int ob_alarm_take(struct ob_alarm_queue *q, struct ob_alarm_event *out) {
    int best = -1;
    int i;
    if (!q || !out) return OB_EINVAL;
    for (i = 0; i < OB_MAX_ALARMS; ++i) {
        struct ob_alarm_event *event = &q->slots[i];
        if (!active(event) || !expired(q, event)) continue;
        if (best < 0 || before(event->deadline, q->slots[best].deadline) ||
            (event->deadline == q->slots[best].deadline &&
             before(event->order, q->slots[best].order))) best = i;
    }
    if (best < 0) return 0;
    *out = q->slots[best];
    ob_zero(&q->slots[best], sizeof(q->slots[best]));
    return 1;
}

int32_t ob_alarm_next_delay(const struct ob_alarm_queue *q) {
    uint32_t remaining;
    uint32_t best = 0;
    int found = 0;
    int i;
    if (!q) return -1;
    for (i = 0; i < OB_MAX_ALARMS; ++i) {
        const struct ob_alarm_event *event = &q->slots[i];
        if (!active(event)) continue;
        if (expired(q, event)) return 0;
        remaining = event->deadline - q->now;
        if (!found || remaining < best) {
            best = remaining;
            found = 1;
        }
    }
    if (!found) return -1;
    return (int32_t)best;
}
