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
// #define LOG_DEBUG_BLOCK_MERGE
// #define LOG_DEBUG_BLOCK

// #define DO_PREV_MERGE
//FIXME: emm..prev merge cannot work..

RefCount alloc_page_cnt;

define_early_init(alloc_page_cnt)
{
    init_rc(&alloc_page_cnt);
}

static QueueNode* phead;
int page_count;
extern char end[];

define_early_init(page_list_init)
{
    for(u64 p = PAGE_BASE((u64) end) + PAGE_SIZE; p < P2K(PHYSTOP); p += PAGE_SIZE) {
        add_to_queue(&phead, (QueueNode*) p);
        page_count++;
    }
}

void* kalloc_page()
{
    _increment_rc(&alloc_page_cnt);
    QueueNode *p = fetch_from_queue(&phead);
    ASSERT(p);

    #ifdef LOG_DEBUG_PAGE
    printk("(CPU %d) Allocated new page at %llx\n", cpuid(), (u64) p);
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
    struct Page_Info *next_page;
    u32 max_size;
    SpinLock page_lock;
};

struct Block_Info
{
    struct Block_Info *prev;
    u32 size;
    u8 used;
};

static struct Page_Info *first_page[4] = {0};

void* kalloc(isize _size)
{
    // Always align to 8 bytes
    u64 size = (_size % 8) ? (_size / 8 + 1) * 8 : _size;
    struct Block_Info *blk;
    struct Page_Info* pg;
    int cid = cpuid();

    // Find a page with enough max_size
    setup_checker(chk1);
    for (pg = first_page[cid]; pg != 0; pg = pg->next_page) {
        if (!try_acquire_spinlock(chk1, &(pg->page_lock))) continue;
        if ((u64) pg->max_size >= size) {
            // Found an available page, now try to find a block
            for (blk = (struct Block_Info*)(pg + 1); 
                ((u64) blk->size < size || blk->used != 0); 
                blk = (struct Block_Info*)((u64) (blk + 1) + (u64) blk->size));

            // Now, blk should point to an available block with enough size
            int need_update = blk->size == pg->max_size;
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
            if (need_update) {
                pg->max_size = 0;
                for (auto b = (struct Block_Info*)(pg + 1); (u64) b < (u64)(pg) + PAGE_SIZE; b = (struct Block_Info*) ((u64) (b + 1) + b->size)) {
                    if (b->used == 0) {
                        pg->max_size = MAX(b->size, pg->max_size);
                    }
                }
            }
            #ifdef LOG_DEBUG_BLOCK
            printk("AB %llx with size %lld, page max_size now %lld\n", (u64) (blk), (u64) blk->size, (u64) pg->max_size);
            #endif

            release_spinlock(chk1, &(pg->page_lock));
            return (void*)(blk + 1);
        }

        // Reach the last page, and still failed to find enough space
        if (pg->next_page == 0) {
            #ifdef LOG_DEBUG_PAGE
            printk("(CPU %d) Reach last page %llx with max size %lld\n", cid, (u64) pg, (u64) pg->max_size);
            #endif

            release_spinlock(chk1, &(pg->page_lock));
            break;
        }
        release_spinlock(chk1, &(pg->page_lock));
    }

    // Need a new page
    struct Page_Info *old_page = pg;
    pg = kalloc_page();
    pg->max_size = PAGE_SIZE - sizeof(struct Page_Info) - sizeof(struct Block_Info);
    pg->next_page = 0;
    init_spinlock(&(pg->page_lock));
    if (first_page[cid] == 0) {
        first_page[cid] = pg;
    }
    else {
        old_page->next_page = pg;
    }

    blk = (struct Block_Info*) (pg + 1);
    blk->prev = blk;
    blk->size = pg->max_size;
    blk->used = 0;
    
    // Divide blk into (size) and another empty block
    if ((u64) blk->size - size <= sizeof(struct Block_Info)) {
        blk->used = 1;
        pg->max_size = 0;
    } else {
        auto nblk = (struct Block_Info*) ((u64) (blk + 1) + size);
        nblk->size = (u64) blk->size - sizeof(struct Block_Info) - size;
        nblk->used = 0;
        nblk->prev = blk;
        blk->used = 1;
        blk->size = size;
        pg->max_size = nblk->size;
    }

    #ifdef LOG_DEBUG_BLOCK
    printk("AB %llx of size %lld in new page %llx, page max_size now %lld\n", (u64) (blk), (u64) blk->size, (u64) pg, (u64) pg->max_size);
    #endif
    return (void*)(blk + 1);
}



void kfree(void* p)
{
    setup_checker(chk2);
    u64 pb = PAGE_BASE((u64) p);
    auto pg = (struct Page_Info*) pb;
    acquire_spinlock(chk2, &(pg->page_lock));
    struct Block_Info* blk = (struct Block_Info*)((u64) p - sizeof(struct Block_Info));
    struct Block_Info *prev = blk->prev;
    struct Block_Info *next = (struct Block_Info*)((u64) (blk + 1) + (u64) blk->size);
    blk->used = 0;
    // prevent unused variable warning..
    (void)next;
    (void)prev;

    #ifdef LOG_DEBUG_BLOCK_MERGE
    printk("Freeing block %llx size %lld...\n", (u64) blk, (u64) blk->size);
    if ((u64) next < pb + PAGE_SIZE) printk("...Next block %llx size %lld, used = %d\n", (u64) next, (u64) next->size, next->used);
    printk("...Previous block %llx size %lld, used = %d\n", (u64) prev, (u64) prev->size, prev->used);
    #endif

    // Check if next is empty
    if ((u64) next < pb + PAGE_SIZE && next->used == 0) {
        blk->size += next->size + sizeof(struct Block_Info);
        auto nnext = (struct Block_Info*) ((u64) (next + 1) + (u64) next->size);
        if ((u64) nnext < pb + PAGE_SIZE) {
            nnext->prev = blk;
        }

        #ifdef LOG_DEBUG_BLOCK_MERGE
        printk("...MN Next block %llx size %lld, merged into block %llx new size %lld\n", (u64) next, (u64) next->size, (u64) blk, (u64) blk->size);
        #endif
    }

    #ifdef DO_PREV_MERGE
    // Check if prev is empty
    next = (struct Block_Info*)((u64) (blk + 1) + (u64) blk->size);
    if (prev != blk && prev->used == 0) {
        printk("...Blk at %llx, prev at %llx\n", (u64) blk, (u64) blk->prev);
        prev->size += blk->size + sizeof(struct Block_Info);
        if ((u64) next < pb + PAGE_SIZE) {
            next->prev = prev;
        }

        #ifdef LOG_DEBUG_BLOCK_MERGE
        printk("...MP Block size is %lld, prev block is %llx new size %lld\n", (u64) blk->size, (u64) prev, (u64) prev->size);
        #endif
    }
    #endif

    // Update max_size of this page
    pg->max_size = MAX(pg->max_size, blk->size);

    // if (pg->max_size == PAGE_SIZE - sizeof(struct Page_Info) - sizeof(struct Block_Info)) {
    //     kfree_page((void*) pg);
    // }
    release_spinlock(chk2, &(pg->page_lock));

    #ifdef LOG_DEBUG_BLOCK
    printk("...Freed block %llx\n", (u64) blk);
    #endif
}
