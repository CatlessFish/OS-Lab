#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <driver/clock.h>
#include <common/rbtree.h>
#include <common/string.h>

// #define DEBUG_LOG_SCHEDLOCKINFO
#define DEBUG_LOG_SCHEDINFO

extern bool panic_flag;
extern struct proc root_proc;
extern struct container root_container;
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

bool _schedtree_node_cmp(rb_node lnode, rb_node rnode) {
    ASSERT(lnode && rnode);
    i64 d = container_of(lnode, struct schinfo, rbnode)->vruntime - container_of(rnode, struct schinfo, rbnode)->vruntime;
    if (d < 0)
        return true;
    if (d == 0)
        return lnode < rnode;
    else return false;
}

void init_schqueue(struct schqueue* sq) {
    memset(&sq->sched_root.rb_node, 0, sizeof(struct rb_root_));
}

struct proc* thisproc()
{
    return cpus[cpuid()].sched.thisproc;
}


void init_schinfo(struct schinfo* p, bool group) {
    p->vruntime = 0;
    p->lastrun = 0;
    p->is_container = group;
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
        struct rb_root_ *schedQ = &p->container->schqueue.sched_root;
        rb_node minNode = _rb_first(schedQ);
        u64 mintime;
        if (minNode == NULL)
            mintime = 0;
        else
            mintime = container_of(minNode, struct schinfo, rbnode)->vruntime;
        p->schinfo.vruntime = mintime;
        ASSERT(_rb_insert(&p->schinfo.rbnode, schedQ, _schedtree_node_cmp) == 0);
        _release_sched_lock();
        return true;
    }
    else return false;
}

void activate_group(struct container* group)
{
    struct container* parent = group->parent;
    _acquire_sched_lock();
    rb_node minNode = _rb_first(&parent->schqueue.sched_root);
    u64 minTime = (minNode == NULL) ? 0 : container_of(minNode, struct schinfo, rbnode)->vruntime;
    group->schinfo.vruntime = minTime;
    ASSERT(_rb_insert(&group->schinfo.rbnode, &parent->schqueue.sched_root, _schedtree_node_cmp) == 0);
    _release_sched_lock();
}

static void update_this_state(enum procstate new_state)
{
    // This rountine is PROTECTED BY SCHED LOCK
    auto p = thisproc();
    ASSERT(p->state == RUNNING);
    p->state = new_state;
    if (!p->idle) {
        // Update vruntime for p and its containers
        u64 time = (p->schinfo.traptime > 0) ? p->schinfo.traptime : get_timestamp_ms();
        u64 run = (p->schinfo.lastrun > 0) ? time - p->schinfo.lastrun : 0;
        p->schinfo.traptime = -1; // in case it stays in kernel mode so that traptime won't be reset
        p->schinfo.lastrun = -1; // in case it goes to sleep but clock still ticking
        p->schinfo.vruntime += run;
        struct container* con = p->container;
        while(run > 0 && con != &root_container) {
            con->schinfo.vruntime += run;
            _rb_erase(&con->schinfo.rbnode, &con->parent->schqueue.sched_root);
            ASSERT(_rb_insert(&con->schinfo.rbnode, &con->parent->schqueue.sched_root, _schedtree_node_cmp) == 0);
            con = con->parent;
        }
        if (new_state == RUNNABLE)
            ASSERT(_rb_insert(&p->schinfo.rbnode, &p->container->schqueue.sched_root, _schedtree_node_cmp) == 0);
    }
}

static rb_node _get_first_runnable(rb_node root) {
    // Sched-lock REQUIRED
    if (root == NULL) return NULL;
    
    // Check left subtree
    rb_node res = _get_first_runnable(root->rb_left);
    if (res != NULL) return res;

    // Then check itself
    auto s = container_of(root, struct schinfo, rbnode);
    if (s->is_container) {
        auto c = container_of(s, struct container, schinfo);
        res = _get_first_runnable(c->schqueue.sched_root.rb_node);
        if (res != NULL) return res;
    }
    else {
        return root;
    }

    // Last check right subtree
    res = _get_first_runnable(root->rb_right);
    return res;
}

static struct proc* pick_next()
{
    // This routine is PROTECTED BY SCHED LOCK
    auto node = _get_first_runnable(root_container.schqueue.sched_root.rb_node);
    if (node == NULL)
        return cpus[cpuid()].sched.idle;

    auto p = container_of(node, struct proc, schinfo.rbnode);
    auto c = p->container;
    _rb_erase(node, &c->schqueue.sched_root);
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
    cpus[cpuid()].sched.thisproc = p;

    // reset schedinfo timer
    p->schinfo.lastrun = get_timestamp_ms();

    // reset cpu sched timer
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

    update_this_state(new_state); // update vruntime, insert node if RUNNABLE && not idle
    auto next = pick_next(); // Choose proc with minimum vruntime, erase node
    ASSERT(next == this || next->state == RUNNABLE);
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

