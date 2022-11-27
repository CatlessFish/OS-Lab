#include <kernel/pid.h>
#include <kernel/mem.h>
#include <kernel/init.h>

int global_pid;
ListNode global_pid_head;
SpinLock pid_lock;

define_early_init(init_global_pid) {
    global_pid = 1;
    init_spinlock(&pid_lock);
    init_list_node(&global_pid_head);
}

void pid_grow(struct container* container) {
    bool global = container == NULL;
    ListNode* head = global ? &global_pid_head : &container->pid_head;
    for (int i = 0; i < 10; i++)
    {
        auto p = (pid_s*) kalloc(sizeof(pid_s));
        p->pid = global ? global_pid++ : container->max_pid++;
        init_list_node(&p->node);
        _insert_into_list(head, &p->node);
    }
}

int pid_get(struct container* container) {
    bool global = container == NULL;
    ListNode* head = global ? &global_pid_head : &container->pid_head;
    _acquire_spinlock(&pid_lock);
    pid_s* p = NULL;
    while (1) {
        _for_in_list(node, head) {
            if (node == head) continue;
            auto cur = container_of(node, pid_s, node);
            if (!cur->used) {
                p = cur;
                break;
            }
        }
        if (p == NULL) pid_grow(container);
        else break;
    }
    _release_spinlock(&pid_lock);
    ASSERT(p != NULL && p->used == 0); // TODO: a bug may occur
    p->used = 1;
    return p->pid;
}

void pid_release(struct container* container, int id) {
    bool global = container == NULL;
    ListNode* head = global ? &global_pid_head : &container->pid_head;
    _acquire_spinlock(&pid_lock);
    _for_in_list(node, head) {
        if (node == head) continue;
        auto p = container_of(node, pid_s, node);
        if (p->pid == id) {
            p->used = 0;
            _detach_from_list(node);
            _insert_into_list(head, node);
            break;
        }
    }
    _release_spinlock(&pid_lock);
}

void pid_compact(struct container* container) {
    bool global = container == NULL;
    ListNode* head = global ? &global_pid_head : &container->pid_head;
    ASSERT(!global);
    _acquire_spinlock(&pid_lock);
    ListNode* cur = head->next;
    while (cur != head) {
        auto p = container_of(cur, pid_s, node);
        if (p->used == 0) {
            cur = _detach_from_list(cur)->next;
            kfree(p);
        }
        else cur = cur->next;
    }
    _release_spinlock(&pid_lock);
}