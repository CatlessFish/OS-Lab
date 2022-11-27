#include <common/string.h>
#include <common/list.h>
#include <kernel/container.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

struct container root_container;
extern struct proc root_proc;

void activate_group(struct container* group);

void set_container_to_this(struct proc* proc)
{
    proc->container = thisproc()->container;
}

void init_container(struct container* container)
{
    memset(container, 0, sizeof(struct container));
    container->parent = NULL;
    container->rootproc = NULL;
    init_schinfo(&container->schinfo, true);
    init_schqueue(&container->schqueue);

    init_list_node(&container->pid_head);
    container->max_pid = 0;
}

struct container* create_container(void (*root_entry)(), u64 arg)
{
    struct proc* this = thisproc();
    struct proc* new_rt = kalloc(sizeof(struct proc));
    struct container* new_con = kalloc(sizeof(struct container));

    init_container(new_con);
    new_con->parent = this->container;
    new_con->rootproc = new_rt;
    
    init_proc(new_rt);
    set_parent_to_this(new_rt);
    new_rt->container = new_con;

    // start_proc will add new_rt into new_con's sched queue
    start_proc(new_rt, root_entry, arg);
    activate_group(new_con);
    return new_con;
}

define_early_init(root_container)
{
    init_container(&root_container);
    root_container.rootproc = &root_proc;
}
