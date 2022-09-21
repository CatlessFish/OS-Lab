#include <common/rc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <common/list.h>
#include <common/spinlock.h>
#include <common/checker.h>
#include <aarch64/mmu.h>
#include <driver/memlayout.h>

// #define LOG_DEBUG_PAGE
// #define LOG_DEBUG_PAGE_MERGE
// #define LOG_DEBUG_BLOCK

RefCount alloc_page_cnt;

define_early_init(alloc_page_cnt)
{
    init_rc(&alloc_page_cnt);
}

static QueueNode* phead;
extern char end[];

define_early_init(page_list_init)
{
    for(u64 p = PAGE_BASE((u64) end) + PAGE_SIZE; p < P2K(PHYSTOP); p += PAGE_SIZE) {
        add_to_queue(&phead, (QueueNode*) p);
    }
}

static SpinLock kalloc_lock;

// define_early_init(spinlock) {
//     init_spinlock(&kalloc_lock);
// }

void* kalloc_page()
{
    _increment_rc(&alloc_page_cnt);
    QueueNode *p = fetch_from_queue(&phead);

    #ifdef LOG_DEBUG_PAGE
    printk("Allocated new page at %llx\n", (u64) p);
    #endif
    
    return (void*) p;
}

void kfree_page(void* p)
{
    _decrement_rc(&alloc_page_cnt);
    add_to_queue(&phead, (QueueNode*) PAGE_BASE((u64) p));

    #ifdef LOG_DEBUG_PAGE
    printk("Freed page %llx\n", PAGE_BASE((u64) p));
    #endif
}

struct Page_Info
{
    struct Page_Info *next_page; // lock required
    u32 max_size;
};

struct Block_Info
{
    struct Block_Info *prev; // lock required
    u32 size;
    u8 used;
};

static struct Page_Info *first_page = 0;

void* kalloc(isize _size)
{
    setup_checker(kallocChecker);
    acquire_spinlock(kallocChecker, &kalloc_lock);
    // Always align to 8 bytes
    u64 size = (_size % 8) ? (_size / 8 + 1) * 8 : _size;

    struct Block_Info *blk;
    struct Page_Info* pg;

    // Find a page with enough max_size
    for (pg = first_page; pg != 0; pg = pg->next_page) {
        if ((u64) pg->max_size >= size) {
            // Found an available page, now try to find a block
            for (blk = (struct Block_Info*)(pg + 1); 
                ((u64) blk->size < size || blk->used != 0); 
                blk = (struct Block_Info*)((u64) (blk + 1) + (u64) blk->size));

            // Now, blk should point to an available block with enough size
            if ((u64) blk->size - size <= sizeof(struct Block_Info)) {
                blk->used = 1;
            } else {
                auto nblk = (struct Block_Info*) ((u64) (blk + 1) + size);
                nblk->size = (u64) blk->size - sizeof(struct Block_Info) - size;
                nblk->used = 0;
                nblk->prev = blk;
                blk->used = 1;
                blk->size = size;
            }

            // Update max_size of this page
            pg->max_size = 0;
            for (auto b = (struct Block_Info*)(pg + 1); (u64) b < (u64)(pg) + PAGE_SIZE; b = (struct Block_Info*) ((u64) (b + 1) + b->size)) {
                if (b->used == 0) {
                    pg->max_size = MAX(b->size, pg->max_size);
                }
            }

            #ifdef LOG_DEBUG_BLOCK
            printk("Allocated block %llx with size %lld, page max_size is now %lld\n", (u64) (blk + 1), (u64) blk->size, (u64) pg->max_size);
            #endif

            release_spinlock(kallocChecker, &kalloc_lock);
            return (void*)(blk + 1);
        }
        // Reach the last page, and still failed to find enough space
        if (pg->next_page == 0) {
            #ifdef LOG_DEBUG_PAGE
            printk("Reach last page %llx with max size %lld\n", (u64) pg, (u64) pg->max_size);
            #endif

            break;
        }
    }

    // Need a new page
    struct Page_Info *old_page = pg;
    pg = kalloc_page();
    old_page->next_page = pg;
    pg->max_size = PAGE_SIZE - sizeof(struct Page_Info) - sizeof(struct Block_Info);
    pg->next_page = 0;

    blk = (struct Block_Info*) (pg + 1);
    blk->prev = blk;
    blk->size = pg->max_size;
    blk->used = 0;
    if (first_page == 0) first_page = pg;
    
    // Divide blk into (size) and another empty block
    if ((u64) blk->size - size <= sizeof(struct Block_Info)) {
        blk->used = 1;
    } else {
        auto nblk = (struct Block_Info*) ((u64) (blk + 1) + size);
        nblk->size = (u64) blk->size - sizeof(struct Block_Info) - size;
        nblk->used = 0;
        nblk->prev = blk;
        blk->used = 1;
        blk->size = size;
    }

    // Update max_size of this page
    pg->max_size = 0;
    for (auto b = (struct Block_Info*) (pg + 1); (u64) b < (u64)(pg) + PAGE_SIZE; b = (struct Block_Info*) ((u64) (b + 1) + (u64) b->size)) {
        if (b->used == 0) 
        {
            pg->max_size = MAX(b->size, pg->max_size);
        }
    }

    #ifdef LOG_DEBUG_BLOCK
    printk("Block %llx of size %lld in new page %llx, page max_size is now %lld\n", (u64) (blk + 1), (u64) blk->size, (u64) pg, (u64) pg->max_size);
    #endif

    release_spinlock(kallocChecker, &kalloc_lock);
    return (void*)(blk + 1);
}

void kfree(void* p)
{
    (void)p;
}

void kfree_1(void* p)
{
    setup_checker(kfreeChecker);
    acquire_spinlock(kfreeChecker, &kalloc_lock);
    u64 pb = PAGE_BASE((u64) p);
    auto pg = (struct Page_Info*) pb;
    struct Block_Info* blk = (struct Block_Info*)((u64) p - sizeof(struct Block_Info));
    blk->used = 0;
    struct Block_Info *prev = blk->prev;
    struct Block_Info *next = (struct Block_Info*)((u64) (blk + 1) + (u64) blk->size);

    // Check if next is empty
    if ((u64) next < pb + PAGE_SIZE && next->used == 0) {
        blk->size += next->size + sizeof(struct Block_Info);
        auto nnext = (struct Block_Info*) ((u64) (next + 1) + next->size);
        if ((u64) nnext < pb + PAGE_SIZE) {
            nnext -> prev = blk;
        }

        #ifdef LOG_DEBUG_BLOCK_MERGE
        printk("Next block %llx with size %lld, merged into block %llx with new size %lld\n", (u64) next, (u64) next->size, (u64) blk, (u64) blk->size);
        #endif
    }

    // Check if prev is empty
    next = (struct Block_Info*)((u64) (blk + 1) + (u64) blk->size);
    if (prev != blk && prev->used == 0) {
        prev->size += blk->size + sizeof(struct Block_Info);
        if ((u64) next < pb + PAGE_SIZE) {
            next->prev = prev;
        }

        #ifdef LOG_DEBUG_BLOCK_MERGE
        printk("Block size is %lld, prev block is %llx with size %lld\n", (u64) blk->size, (u64) prev, (u64) prev->size);
        #endif
    }

    // Update max_size of this page
    pg->max_size = 0;
    for (auto b = (struct Block_Info*) (pg + 1); (u64) b < pb + PAGE_SIZE; b = (struct Block_Info*) ((u64) (b + 1) + (u64) b->size)) {
        if (b->used == 0) {
            pg->max_size = MAX(pg->max_size, b->size);
        }
    }
    // For debugging...
    // if (pg->max_size > PAGE_SIZE) {
    //     printk("Page %llx Size ERROR with size %lld\n", pb, (u64) pg->max_size);
    //     for (auto b = (struct Block_Info*) (pg + 1); (u64) b < pb + PAGE_SIZE; b = (struct Block_Info*) ((u64) (b + 1) + (u64) b->size)) {
    //         {
    //             printk("Block %llx with size %lld, used=%d\n", (u64) b, (u64) b->size, b->used);
    //         }
    // }
    // }

    if (pg->max_size == PAGE_SIZE - sizeof(struct Page_Info)) {
        kfree_page((void*) pg);
    }
    release_spinlock(kfreeChecker, &kalloc_lock);

    #ifdef LOG_DEBUG_BLOCK
    printk("Freed block %llx\n", (u64) blk);
    #endif
}
