#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <kernel/schinfo.h>

enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, ZOMBIE };

typedef struct UserContext
{
    // TODO: customize your trap frame
    u64 spsr, elr;
    // x0~x17
    u64 caller_saved_regs[18];
} UserContext;

typedef struct KernelContext
{
    // TODO: customize your context
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
    void* kstack;
    UserContext* ucontext;
    KernelContext* kcontext;
};

// void init_proc(struct proc*);
struct proc* create_proc();
int start_proc(struct proc*, void(*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
int wait(int* exitcode);
