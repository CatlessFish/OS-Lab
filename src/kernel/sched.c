#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <driver/clock.h>

// #define DEBUG_LOG_SCHEDLOCKINFO
// #define DEBUG_LOG_SCHEDINFO

extern bool panic_flag;

extern void swtch(KernelContext* new_ctx, KernelContext** old_ctx);

ListNode run_queue;
ListNode* last_pick;

define_early_init(init_run_queue) {
    init_list_node(&run_queue);
    last_pick = &run_queue;
}

define_init(sched_init) {
    for (int i = 0; i < NCPU; i++) {
        struct proc* p = kalloc(sizeof(struct proc));
        p->idle = true;
        p->state = RUNNING;
        cpus[i].sched.idle = p;
        cpus[i].sched.thisproc = p;
        // idle may need its schinfo, if a sheduler uses idle
    }
}

struct proc* thisproc()
{
    // TODO: return the current process
    return cpus[cpuid()].sched.thisproc;
}

void init_schinfo(struct schinfo* p)
{
    // TODO: initialize your customized schinfo for every newly-created process
    init_list_node(&p->sch_node);
}

SpinLock sched_lock;
define_early_init(init_sched_lock) {
    init_spinlock(&sched_lock);
}

void _acquire_sched_lock()
{
    // TODO: acquire the sched_lock if need
    _acquire_spinlock(&sched_lock);

    #ifdef DEBUG_LOG_SCHEDLOCKINFO
    printk("+CPU %d Acquired sched lock\n", cpuid());
    #endif
}

void _release_sched_lock()
{
    // TODO: release the sched_lock if need
    _release_spinlock(&sched_lock);

    #ifdef DEBUG_LOG_SCHEDLOCKINFO
    printk("-CPU %d Released sched lock\n", cpuid());
    #endif
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
    r = p->state == ZOMBIE;
    _release_sched_lock();
    return r;
}

bool activate_proc(struct proc* p)
{
    // TODO
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else: panic
    if (p->state == RUNNABLE || p->state == RUNNING) return false;
    else if (p->state == SLEEPING || p->state == UNUSED)
    {
        _acquire_sched_lock();
        p->state = RUNNABLE;
        _insert_into_list(&run_queue, &p->schinfo.sch_node);
        _release_sched_lock();
        return true;
    }
    else PANIC();
    
}

static void update_this_state(enum procstate new_state)
{
    // TODO: if using simple_sched, you should implement this routinue
    // update the state of current process to new_state, and remove it from the sched queue if new_state=SLEEPING/ZOMBIE
    // _acquire_sched_lock();
    auto p = thisproc();
    p->state = new_state;
    if (new_state == SLEEPING || new_state == ZOMBIE) {
        _detach_from_list(&p->schinfo.sch_node);
    }
    // _release_sched_lock();
}

extern bool panic_flag;

static struct proc* pick_next()
{
    // TODO: if using simple_sched, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    if (panic_flag) return cpus[cpuid()].sched.idle;
    struct proc* next = NULL;
    _for_in_list(node, last_pick->next) {
        if (node == &run_queue) continue;
        auto p = container_of(node, struct proc, schinfo.sch_node);
        if (p->state == RUNNABLE) {
            next = p;
            last_pick = node;
            break;
        }
    }
    return next ? next : cpus[cpuid()].sched.idle;
}

static void update_this_proc(struct proc* p)
{
    // TODO: if using simple_sched, you should implement this routinue
    // update thisproc to the choosen process, and reset the clock interrupt if need
    reset_clock(1000);
    cpus[cpuid()].sched.thisproc = p;
}

void
print_run_queue_info() {
    _for_in_list(node, &run_queue) {
        if (node == &run_queue) continue;
        auto p = container_of(node, struct proc, schinfo.sch_node);
        printk("pid: %d, state: %d\n", p->pid, p->state);
    }
    printk("End of rq\n");
}

extern struct proc root_proc;

// A simple scheduler.
// You are allowed to replace it with whatever you like.
static void simple_sched(enum procstate new_state)
{
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    auto next = pick_next();
    update_this_state(new_state);
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this)
    {
        #ifdef DEBUG_LOG_SCHEDINFO
        printk("CPU %d: pid %d switch to pid %d\n", cpuid(), this->pid, next->pid);
        #endif

        swtch(next->kcontext, &this->kcontext);
    } else {
        #ifdef DEBUG_LOG_SCHEDINFO
        printk("Picked this, schedule canceled, cpu %d, pid %d\n", cpuid(), this->pid);
        print_run_queue_info();
        printk("root state: %d\n", root_proc.state);
        delay_us(100000);
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

