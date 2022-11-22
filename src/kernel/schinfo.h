#pragma once

#include <common/list.h>
#include <common/rbtree.h>
struct proc; // dont include proc.h here

// embedded data for cpus
struct sched
{
    // customize your sched info
    struct proc* thisproc;
    struct proc* idle;
};

// embeded data for procs
struct schinfo
{
    // customize your sched info
    ListNode sch_node;
    struct rb_node_ rbnode;
    u64 vruntime;
    u64 lastrun;
};

// embedded data for containers
struct schqueue
{
    // TODO: customize your sched queue
    
};
