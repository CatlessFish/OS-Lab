#include <kernel/proc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <kernel/pid.h>
#include <common/list.h>
#include <common/string.h>
#include <common/spinlock.h>
#include <kernel/printk.h>

extern struct container root_container;

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


void kernel_entry();
void proc_entry();

void set_parent_to_this(struct proc* proc)
{
    // set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL

    _acquire_proc_lock();
    proc->parent = thisproc();
    _insert_into_list(&thisproc()->children, &proc->ptnode);
    _release_proc_lock();
}

NO_RETURN void exit(int code)
{
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the rootproc of the container, and notify the it if there is zombie
    // 4. notify the parent
    // 5. sched(ZOMBIE)
    // NOTE: be careful of concurrency

    struct proc* this = thisproc();
    ASSERT (this != this->container->rootproc && !this->idle);
    this->exitcode = code;
    free_pgdir(&this->pgdir);
    _acquire_proc_lock();
    struct proc* rp = this->container->rootproc;
    for (auto p = this->children.next; p != &this->children; ) {
        if (p == &this->children) continue;
        struct proc *child = container_of(p, struct proc, ptnode);
        child->parent = rp;
        p = _detach_from_list(&child->ptnode); // child->ptnode == p;
        p = p->next;
        _insert_into_list(&rp->children, &child->ptnode);
        if (is_zombie(child)) {
            post_sem(&rp->childexit);
        }
    }
    // Parent should be RUNNING, RUNNABLE or SLEEPING
    ASSERT(this->parent->state >= 1 && this->parent->state <= 4);
    post_sem(&this->parent->childexit);
    _acquire_sched_lock();

    #ifdef DEBUG_LOG_EXITINFO
    printk("Exited: CPU %d pid %d\n", cpuid(), this->pid);
    #endif

    _release_proc_lock();
    _sched(ZOMBIE);
    
    PANIC(); // prevent the warning of 'no_return function returns'
}

int wait(int* exitcode, int* pid)
{
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its local pid and exitcode
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
                *pid = child->pid;
                int pid = child->localpid;
                pid_release(child->container, child->localpid);
                kfree_page(child->kstack);
                p = _detach_from_list(&child->ptnode);
                kfree(child);
                _release_proc_lock();
                pid_release(child->container, pid);
                return pid;
            }
        }
        _release_proc_lock();
        // yield();
        printk("Waiting.. CPU %d pid %d\n", cpuid(), thisproc()->pid);
    // }
    PANIC();
}

bool found, fail;
int target_id;

static struct proc* dfs_proctree(ListNode* p) {
    struct proc* cur = container_of(p, struct proc, ptnode);
    if (cur->pid == target_id) {
        if (is_unused(cur)) {
            fail = true;
            return NULL;
        } else {
            found = true;
            return cur;
        }
    }
    _for_in_list(node, &cur->children) {
        if (node == &cur->children) continue;
        auto res = dfs_proctree(node);
        if (found) return res;
        if (fail) return NULL;
    }
    return NULL;
}

int kill(int pid)
{
    // Set the killed flag of the proc to true and return 0.
    // Return -1 if the pid is invalid (proc not found).

    bool kill = false; // Avoid access to global found after releasing proc_lock
    _acquire_proc_lock();
    found = false;
    fail = false;
    target_id = pid;
    struct proc* p = dfs_proctree(&root_proc.ptnode);
    if (found) {
        ASSERT(p);
        p->killed = 1;
        kill = true;
    }
    _release_proc_lock();
    if (kill) {
        alert_proc(p);
        return 0;
    }
    else return -1;
}

int start_proc(struct proc* p, void(*entry)(u64), u64 arg)
{
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its local pid
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
    p->localpid = pid_get(p->container);
    int id = p->localpid;
    ASSERT(p->parent != NULL);
    activate_proc(p); // after this, p should not be used
    return id;
}

void init_proc(struct proc* p)
{
    // setup the struct proc with kstack and pid allocated
    // NOTE: be careful of concurrency

    memset(p, 0, sizeof(*p));
    p->killed = 0;
    p->idle = 0;
    p->container = &root_container;
    p->pid = pid_get(NULL);
    p->exitcode = 0;
    p->state = UNUSED;
    init_pgdir(&p->pgdir);
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    init_schinfo(&p->schinfo, false);
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
