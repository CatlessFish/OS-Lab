#pragma once

#include <common/list.h>
#include <common/spinlock.h>
#include <kernel/container.h>


typedef struct {
    bool used;
    int pid;
    ListNode node;
} pid_s;

void pid_grow(struct container* container);
int pid_get(struct container* container);
void pid_release(struct container* container, int id);
void pid_compact(struct container* container);
