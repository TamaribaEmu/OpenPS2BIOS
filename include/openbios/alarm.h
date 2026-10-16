/* SPDX-License-Identifier: MIT */
#ifndef OPENBIOS_ALARM_H
#define OPENBIOS_ALARM_H

#include "base.h"

#define OB_MAX_ALARMS 64

struct ob_alarm_event {
    int32_t id;
    uint32_t callback, common, gp;
    uint16_t time;
    uint32_t deadline, order;
};

struct ob_alarm_queue {
    uint32_t now, next_id, sequence;
    struct ob_alarm_event slots[OB_MAX_ALARMS];
};

void ob_alarm_init(struct ob_alarm_queue *q);
int ob_alarm_set(struct ob_alarm_queue *q, uint16_t time, uint32_t callback,
                 uint32_t common, uint32_t gp);
int ob_alarm_release(struct ob_alarm_queue *q, int id);
void ob_alarm_advance(struct ob_alarm_queue *q, uint32_t elapsed);
int ob_alarm_take(struct ob_alarm_queue *q, struct ob_alarm_event *out);
int32_t ob_alarm_next_delay(const struct ob_alarm_queue *q);

#endif
