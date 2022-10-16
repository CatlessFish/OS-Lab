#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <common/list.h>
#include <common/string.h>
#include <common/spinlock.h>
#include <kernel/printk.h>

struct proc root_proc;

// #define DEBUG_LOG_PROCLOCKINFO
// #define DEBUG_LOG_EXITINFO

SpinLock proc_tree_lock;
define_early_init(init_proc_lock) {
    init_spinlock(&proc_tree_lock);
}

void _acquire_proc_lock() {
    _acquire_spinlock(&proc_tree_lock);

    #ifdef DEBUG_LOG_PROCLOCKINFO
    printk("++CPU %d Acquired proc lock\n", cpuid());
    #endif
}

void _release_proc_lock() {
    _release_spinlock(&proc_tree_lock);

    #ifdef DEBUG_LOG_PROCLOCKINFO
    printk("--CPU %d Released proc lock\n", cpuid());
    #endif
}


typedef struct {
    bool used;
    int pid;
    ListNode node;
} pid_s;

int pid;
ListNode pid_head;
SpinLock pid_lock;

define_early_init(init_pid) {
    pid = 1;
    init_spinlock(&pid_lock);
    init_list_node(&pid_head);
}

void pid_grow() {
    for (int i = 0; i < 10; i++)
    {
        auto p = (pid_s*) kalloc(sizeof(pid_s));
        p->pid = pid++;
        init_list_node(&p->node);
        _insert_into_list(&pid_head, &p->node);
    }
}

int get_pid() {
    _acquire_spinlock(&pid_lock);
    pid_s* p = NULL;
    while (1) {
        _for_in_list(node, &pid_head) {
            if (node == &pid_head) continue;
            auto cur = container_of(node, pid_s, node);
            if (!cur->used) {
                p = cur;
                break;
            }
        }
        if (p == NULL) pid_grow();
        else break;
    }
    _release_spinlock(&pid_lock);
    ASSERT(p != NULL && p->used == 0); // TODO: a bug may occur
    p->used = 1;
    return p->pid;
}

void release_pid(int id) {
    _acquire_spinlock(&pid_lock);
    _for_in_list(node, &pid_head) {
        if (node == &pid_head) continue;
        auto p = container_of(node, pid_s, node);
        if (p->pid == id) {
            p->used = 0;
            break;
        }
    }
    _release_spinlock(&pid_lock);
}

void kernel_entry();
void proc_entry();

void set_parent_to_this(struct proc* proc)
{
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL

    _acquire_proc_lock();
    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
    _release_proc_lock();
}

NO_RETURN void exit(int code)
{
    // TODO
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
    // 4. sched(ZOMBIE)
    // NOTE: be careful of concurrency

    thisproc()->exitcode = code;
    // Any resources to be cleaned?
    _acquire_proc_lock();
    for (auto p = thisproc()->children.next; p != &thisproc()->children; ) {
        if (p == &thisproc()->children) continue;
        struct proc *child = container_of(p, struct proc, ptnode);
        child->parent = &root_proc;
        p = _detach_from_list(&child->ptnode); // child->ptnode == p;
        p = p->next;
        _insert_into_list(&root_proc.children, &child->ptnode);
        if (is_zombie(child)) {
            post_sem(&(root_proc.childexit));
        }
    }
    // Parent should be RUNNING, RUNNABLE or SLEEPING
    ASSERT(thisproc()->parent->state >= 1 && thisproc()->parent->state <= 3);
    post_sem(&thisproc()->parent->childexit);
    _acquire_sched_lock();

    #ifdef DEBUG_LOG_EXITINFO
    printk("Exited: CPU %d pid %d\n", cpuid(), thisproc()->pid);
    #endif

    _release_proc_lock();
    _sched(ZOMBIE);
    
    PANIC(); // prevent the warning of 'no_return function returns'
}

extern SpinLock sched_lock;

int wait(int* exitcode)
{
    // TODO
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its pid and exitcode
    // NOTE: be careful of concurrency

    if (_empty_list(&thisproc()->children)) return -1;
    bool v = wait_sem(&thisproc()->childexit);
    ASSERT(v);

    // while (1) {
        _acquire_proc_lock();
        _for_in_list(p, &thisproc()->children) {
            if (p == &thisproc()->children) continue;
            struct proc *child = container_of(p, struct proc, ptnode);
            // ASSERT(child->state <= 4 && child->state >= 0);
            if (is_zombie(child)) {
                *exitcode = child->exitcode;
                int pid = child->pid;
                kfree_page(child->kstack);
                p = _detach_from_list(&child->ptnode);
                kfree(child);
                _release_proc_lock();
                release_pid(pid);
                return pid;
            }
        }
        _release_proc_lock();
        // yield();
        printk("Waiting.. CPU %d pid %d\n", cpuid(), thisproc()->pid);
    // }
    PANIC();
}

int start_proc(struct proc* p, void(*entry)(u64), u64 arg)
{
    // TODO
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its pid
    // NOTE: be careful of concurrency

    if (p->parent == NULL) {
        _acquire_proc_lock();
        p->parent = &root_proc;
        _insert_into_list(&(root_proc.children), &p->ptnode);
        _release_proc_lock();
    }
    p->kcontext->lr = (u64) &proc_entry;
    p->kcontext->x0 = (u64) entry;
    p->kcontext->x1 = (u64) arg;
    int id = p->pid;
    ASSERT(p->parent != NULL);
    activate_proc(p); // after this, p should not be used
    return id;
}

void init_proc(struct proc* p)
{
    // TODO
    // setup the struct proc with kstack and pid allocated
    // NOTE: be careful of concurrency

    memset(p, 0, sizeof(*p));
    p->killed = 0;
    p->idle = 0;
    p->pid = get_pid();
    p->exitcode = 0;
    p->state = UNUSED;
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    init_schinfo(&p->schinfo);
    p->kstack = kalloc_page();
    ASSERT(p->kstack);
    p->ucontext = (UserContext*) ((u64) p->kstack + PAGE_SIZE - 16 - sizeof(UserContext));
    p->kcontext = (KernelContext*) ((u64) p->ucontext - sizeof(KernelContext));
}

struct proc* create_proc()
{
    struct proc* p = kalloc(sizeof(struct proc));
    init_proc(p);
    return p;
}

void test_proc()
{
    printk("test\n");
    exit(0);
}

define_init(root_proc)
{
    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    // start_proc(&root_proc, test_proc, 123);
    start_proc(&root_proc, kernel_entry, 123456);
}
