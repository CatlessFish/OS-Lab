#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <driver/clock.h>
#include <common/rbtree.h>

// #define DEBUG_LOG_SCHEDLOCKINFO
// #define DEBUG_LOG_SCHEDINFO

extern bool panic_flag;
extern struct proc root_proc;
extern struct timer sched_timer[4];

extern void swtch(KernelContext* new_ctx, KernelContext** old_ctx);

SpinLock sched_lock;
define_early_init(init_sched_lock) {
    init_spinlock(&sched_lock);
}

void _acquire_sched_lock()
{
    // acquire the sched_lock if need
    _acquire_spinlock(&sched_lock);

    #ifdef DEBUG_LOG_SCHEDLOCKINFO
    printk("+CPU %d Acquired sched lock\n", cpuid());
    #endif
}

void _release_sched_lock()
{
    // release the sched_lock if need
    _release_spinlock(&sched_lock);

    #ifdef DEBUG_LOG_SCHEDLOCKINFO
    printk("-CPU %d Released sched lock\n", cpuid());
    #endif
}


// CFS Scheduler
void _acquire_schedtree_lock() {
    _acquire_sched_lock();
}

void _release_schedtree_lock() {
    _release_sched_lock();
}

static struct rb_root_ sched_root;

bool _schedtree_node_cmp(rb_node lnode, rb_node rnode) {
    ASSERT(lnode && rnode);
    i64 d = container_of(lnode, struct schinfo, rbnode)->vruntime - container_of(rnode, struct schinfo, rbnode)->vruntime;
    if (d < 0)
        return true;
    if (d == 0)
        return lnode < rnode;
    else return false;
}

define_init(idle_init) {
    for (int i = 0; i < NCPU; i++) {
        struct proc* p = kalloc(sizeof(struct proc));
        ASSERT(p);
        p->idle = true;
        p->state = RUNNING;
        cpus[i].sched.idle = p;
        cpus[i].sched.thisproc = p;
        // idle may need its schinfo, if a sheduler uses idle
    }
}

struct proc* thisproc()
{
    return cpus[cpuid()].sched.thisproc;
}


void init_schinfo(struct schinfo* p, bool group) {
    p->vruntime = 0;
    p->lastrun = 0;
}

bool is_zombie(struct proc* p)
{
    bool r;
    _acquire_sched_lock();
    r = p->state == ZOMBIE;
    _release_sched_lock();
    return r;
}

bool is_unused(struct proc* p)
{
    bool r;
    _acquire_sched_lock();
    r = p->state == UNUSED;
    _release_sched_lock();
    return r;
}

bool _activate_proc(struct proc* p, bool onalert)
{
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else(ZOMBIE): return false 
    if (p->state == RUNNABLE || p->state == RUNNING) return false;
    else if (p->state == SLEEPING || p->state == UNUSED || p->state == DEEPSLEEPING)
    {   
        if (p->state == DEEPSLEEPING && onalert == true) return false;
        _acquire_sched_lock();
        p->state = RUNNABLE;
        u64 mintime;
        rb_node minNode = _rb_first(&sched_root);
        if (minNode == NULL)
            mintime = 0;
        else
            mintime = container_of(minNode, struct schinfo, rbnode)->vruntime;
        p->schinfo.vruntime = mintime;
        ASSERT(_rb_insert(&p->schinfo.rbnode, &sched_root, _schedtree_node_cmp) == 0);
        _release_sched_lock();
        return true;
    }
    else return false;
}

void activate_group(struct container* group)
{
    // TODO: add the schinfo node of the group to the schqueue of its parent

}

static void update_this_state(enum procstate new_state)
{
    // This rountine is PROTECTED BY SCHED LOCK
    auto p = thisproc();
    ASSERT(p->state == RUNNING);
    p->state = new_state;
    if (!p->idle) {
        // auto check = _rb_lookup(&p->schinfo.rbnode, &sched_root, _schedtree_node_cmp);
        // ASSERT(!check);
        u64 run = get_timestamp_ms() - p->schinfo.lastrun;
        p->schinfo.vruntime += run;
        if (new_state == RUNNABLE)
            ASSERT(_rb_insert(&p->schinfo.rbnode, &sched_root, _schedtree_node_cmp) == 0);
    }
}

static struct proc* pick_next()
{
    // This routine is PROTECTED BY SCHED LOCK
    auto node = _rb_first(&sched_root);
    if (node == NULL) 
        return cpus[cpuid()].sched.idle;
    _rb_erase(node, &sched_root);
    auto p = container_of(node, struct proc, schinfo.rbnode);
    return p;
}

static bool __my_timer_cmp(rb_node lnode, rb_node rnode) {
    i64 d = container_of(lnode, struct timer, _node)->_key - container_of(rnode, struct timer, _node)->_key;
    if (d < 0)
        return true;
    if (d == 0)
        return lnode < rnode;
    return false;
}

static void update_this_proc(struct proc* p)
{
    // This routine is PROTECTED BY SCHED LOCK
    // update thisproc to the choosen process, and reset the clock interrupt if need
    p->state = RUNNING;
    p->schinfo.lastrun = get_timestamp_ms();
    cpus[cpuid()].sched.thisproc = p;
    auto check = _rb_lookup(&(sched_timer[cpuid()]._node), &(cpus[cpuid()].timer), __my_timer_cmp);
    if (check) 
        cancel_cpu_timer(&sched_timer[cpuid()]);
    set_cpu_timer(&sched_timer[cpuid()]);
}

static void simple_sched(enum procstate new_state)
{
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    if (this->killed && new_state != ZOMBIE) {
        _release_sched_lock();
        return;
    }

    auto next = pick_next(); // Choose proc with minimum vruntime, erase node
    ASSERT(next == this || next->state == RUNNABLE);
    update_this_state(new_state); // update vruntime, insert node if RUNNABLE && not idle
    update_this_proc(next); // set next.lastrun, set cpu sched timer
    if (next != this)
    {
        #ifdef DEBUG_LOG_SCHEDINFO
        printk("CPU %d: pid %d switch to pid %d\n", cpuid(), this->pid, next->pid);
        #endif
        attach_pgdir(&next->pgdir);
        swtch(next->kcontext, &this->kcontext);
    } else {
        #ifdef DEBUG_LOG_SCHEDINFO
        printk("Picked this, schedule canceled, cpu %d, pid %d\n", cpuid(), this->pid);
        #endif
    }
    if (!thisproc()->idle) ASSERT(thisproc()->parent->state >= 1 && thisproc()->parent->state <= 4);
    _release_sched_lock();
}

__attribute__((weak, alias("simple_sched"))) void _sched(enum procstate new_state);

u64 proc_entry(void(*entry)(u64), u64 arg)
{
    _release_sched_lock();
    set_return_addr(entry);
    return arg;
}

