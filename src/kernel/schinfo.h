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
    struct rb_node_ rbnode;
    // Accumulative virtual runtime
    u64 vruntime;
    // Timestamp of the last scheduled time
    u64 lastrun;
    // Timestamp of when it was trapped in from userspace
    u64 traptime;
    bool is_container;
};

// embedded data for containers
struct schqueue
{
    // TODO: customize your sched queue
    struct rb_root_ sched_root;
    // All schedqueue use the same global sched_lock
    // bool (*cmp)(rb_node lnode, rb_node rnode);
};

// cmp function for sched rb-tree
bool _schedtree_node_cmp(rb_node lnode, rb_node rnode);
