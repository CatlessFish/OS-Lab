#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <kernel/schinfo.h>
#include <kernel/pt.h>

enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, ZOMBIE };

typedef struct UserContext
{
    // customize your trap frame
    u64 spsr, elr;
    // x0~x17
    u64 x[18];
    u64 ttbr0;
} UserContext;

typedef struct KernelContext
{
    // customize your context
    // x29 LR
    u64 lr;
    u64 x0, x1;
    // x19~x29
    u64 callee_saved_regs[11];
} KernelContext;

struct proc
{
    bool killed;
    bool idle;
    int pid;
    int exitcode;
    enum procstate state;
    Semaphore childexit;
    ListNode children;
    ListNode ptnode;
    struct proc* parent;
    struct schinfo schinfo;
    struct pgdir pgdir;
    void* kstack;
    UserContext* ucontext;
    KernelContext* kcontext;
};

// void init_proc(struct proc*);
struct proc* create_proc();
int start_proc(struct proc*, void(*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
int wait(int* exitcode);
int kill(int pid);
