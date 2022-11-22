#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>

// #define LOG_DEBUG_BLOCKINFO
// #define LOG_DEBUG_ABSORPTION

static const SuperBlock* sblock;
static const BlockDevice* device;

#define MAX_LOG_BLOCKS 200 // Reserved space for logged blocks in `end_op`

static SpinLock lock;     // protects block cache.
static ListNode head;     // the list of all allocated in-memory block.
static LogHeader header;  // in-memory copy of log header block.

static SpinLock op_num_lock; // protects these three below
static int running_op_num;
static int remaining_log_num;
static SleepLock op_available;

static SpinLock op_head_lock;
static ListNode op_head;
static usize bm_bno;  // block no for bitmap
static usize log_start;

// hint: you may need some other variables. Just add them here.
struct LOG {
    SpinLock lock;
    int bno[LOG_MAX_SIZE]; // block number
    int num_blocks;
} log;

// read the content from disk.
static INLINE void device_read(Block* block) {
    device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block* block) {
    device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header() {
    device->read(sblock->log_start, (u8*)&header);
}

// write log header back to disk.
static INLINE void write_header() {
    device->write(sblock->log_start, (u8*)&header);
}

// initialize a block struct.
static void init_block(Block* block) {
    block->block_no = 0;
    init_list_node(&block->node);
    block->acquired = false;
    block->pinned = false;
    block->pending = 0;

    init_sleeplock(&block->lock);
    block->valid = false;
    memset(block->data, 0, sizeof(block->data));
}

// find a block in cache to evict.
// Caller must hold lock.
static Block* get_next_to_evict() {
    // LRU strategy, look up for block from behind
    ListNode *node;
    Block *target = NULL;
    for (node = head.prev; node != &head; node = node->prev) {
        auto b = container_of(node, Block, node);
        if (b->acquired == false && b->pinned == false && b->pending == 0) {
            // ASSERT(b->lock.val == 1);
            target = b;
            break;
        }
    }
    // return NULL if no available block found
    return target;
}

// evict block from cache, and free that block. 
// Caller must hold lock and gaurantee the block is free to evict.
static bool try_evict_block(Block* target) {
    if (target == NULL) return false;
    ASSERT(target->acquired == false);
    _detach_from_list(&target->node);
    kfree(target);
    return true;
}

// no lock version
static usize _get_num_cached_blocks() {
    int count = 0;
    _for_in_list(node, &head) {
        if (node == &head) continue;
        count++;
    }
    return count;
}

// see `cache.h`.
static usize get_num_cached_blocks() {
    _acquire_spinlock(&lock);
    int count = _get_num_cached_blocks();
    _release_spinlock(&lock);
    return count;
}

// see `cache.h`.
static Block* cache_acquire(usize block_no) {
    Block *target = NULL;
    _acquire_spinlock(&lock);
    _for_in_list(node, &head) {
        if (node == &head) continue;
        auto b = container_of(node, Block, node);
        if (b->block_no == block_no) {
            target = b;
            break;
        }
    }
    if (target) {
        // check target->lock
        target->pending++;
        _release_spinlock(&lock);
        unalertable_wait_sem(&target->lock);
        _acquire_spinlock(&lock);
        target->pending--;
        ASSERT(target->acquired == false);
        target->acquired = true;

        // Update LRU
        _detach_from_list(&target->node);
        _insert_into_list(&head, &target->node);
        while (_get_num_cached_blocks() > EVICTION_THRESHOLD) {
            bool ok;
            ok = try_evict_block(get_next_to_evict());
            if (!ok) break;
        }
        _release_spinlock(&lock);
        return target;
    }
    
    // Block not cached, read from device
    target = kalloc(sizeof(Block));
    init_block(target);
    target->block_no = block_no;
    device_read(target);
    target->valid = true;
    target->acquired = true;
    wait_sem(&target->lock);
    _insert_into_list(&head, &target->node);

    while (_get_num_cached_blocks() > EVICTION_THRESHOLD) {
            bool ok;
            ok = try_evict_block(get_next_to_evict());
            if (!ok) break;
    }
    _release_spinlock(&lock);
    return target;
}

// see `cache.h`.
static void cache_release(Block* block) {
    _acquire_spinlock(&lock);
    block->acquired = false;
    _release_spinlock(&lock);
    post_sem(&block->lock);

}

// initialize block cache.
void init_bcache(const SuperBlock* _sblock, const BlockDevice* _device) {
    sblock = _sblock;
    device = _device;

    // init bitmap and log
    bm_bno = sblock->bitmap_start;
    log_start = sblock->log_start;
    init_spinlock(&log.lock);
    memset(log.bno, 0, sizeof(log.bno));
    log.num_blocks = 0;

    // init op
    running_op_num = 0;
    remaining_log_num = MIN(sblock->num_log_blocks, LOG_MAX_SIZE);
    init_spinlock(&op_num_lock);
    init_sleeplock(&op_available);
    init_list_node(&op_head);
    init_spinlock(&op_head_lock);

    // init head, lock and header
    init_list_node(&head);
    init_spinlock(&lock);
    memset(&header, 0, sizeof(LogHeader));

    // replay
    read_header();
    if (header.num_blocks) {
        for (int i = 0; i < header.num_blocks; i++) {
            u8* data[512];
            device->read(log_start + 1 + i, data);
            device->write(header.block_no[i], data);
        }
        header.num_blocks = 0;
    }
    memset(&header, 0, sizeof(header));
    write_header();
}

// see `cache.h`.
static void cache_begin_op(OpContext* ctx) {
    while (true) {
        unalertable_wait_sem(&op_available);
        _acquire_spinlock(&op_num_lock);
        if (remaining_log_num >= OP_MAX_NUM_BLOCKS) {
            remaining_log_num -= OP_MAX_NUM_BLOCKS;
            if (remaining_log_num >= OP_MAX_NUM_BLOCKS)
                post_sem(&op_available);
            break;
        }
        _release_spinlock(&op_num_lock);
    }
    
    running_op_num++;
    _release_spinlock(&op_num_lock);
    ctx->rm = OP_MAX_NUM_BLOCKS;
    ctx->num_blocks = 0;
    memset(ctx->bno, 0, sizeof(ctx->bno));
    init_list_node(&ctx->node);
    init_sem(&ctx->ok, 0);
    init_spinlock(&ctx->lock);
}

// see `cache.h`.
static void cache_sync(OpContext* ctx, Block* block) {
    if (ctx == NULL) {
        device_write(block);
        return;
    }
    _acquire_spinlock(&ctx->lock);
    int pos = 0;
    for (pos = 0; pos < ctx->num_blocks; pos++) {
        // Local absorption
        if (ctx->bno[pos] == block->block_no) break;
    }
    if (pos == OP_MAX_NUM_BLOCKS) {
        // Too many blocks!
        PANIC();
    } else if (pos == ctx->num_blocks) {
        ASSERT(ctx->num_blocks < OP_MAX_NUM_BLOCKS);
        ctx->bno[ctx->num_blocks++] = block->block_no;
    }
    block->pinned = true;
    _release_spinlock(&ctx->lock);
}

// see `cache.h`.
static void cache_end_op(OpContext* ctx) {
    // Global absorption
    _acquire_spinlock(&log.lock);
    int absorbed = 0;
    for(int i = 0; i < ctx->num_blocks; i++) {
        int flag = 0;
        for (int j = 0; j < log.num_blocks; j++) {
            if (ctx->bno[i] == log.bno[j]) {
                absorbed++;
                flag = 1;
                break;
            }
        }
        if (flag) continue;
        ASSERT(log.num_blocks < LOG_MAX_SIZE - 1);
        log.bno[log.num_blocks++] = ctx->bno[i];
    }
    _release_spinlock(&log.lock);

    insert_into_list(&op_head_lock, &op_head, &ctx->node);
    _acquire_spinlock(&op_num_lock);
    int reuse = OP_MAX_NUM_BLOCKS - (ctx->num_blocks - absorbed);
    remaining_log_num += reuse;
    // printk("reuse: %d\n", reuse);
    if (remaining_log_num - reuse < OP_MAX_NUM_BLOCKS && remaining_log_num >= OP_MAX_NUM_BLOCKS) {
        // printk("Remaining now %d, op_avai %d, up\n", remaining_log_num, _query_sem(&op_available));
        post_sem(&op_available);
    }
    running_op_num--;

    // Check if we need to commit
    if (running_op_num == 0) {
        // Last outstanding op. Now commit.
        _acquire_spinlock(&log.lock);

        // 1. Write into on-disk log blocks and then on-disk log header
        Block* bp[MAX_LOG_BLOCKS];
        memset(bp, 0, sizeof(bp));
        for (int i = 0; i < log.num_blocks; i++) {
            Block *b = cache_acquire(log.bno[i]);
            bp[i] = b;
            device->write(log_start + 1 + i, b->data);
            header.block_no[i] = log.bno[i];
        }

        arch_fence();
        header.num_blocks = log.num_blocks;
        arch_fence();
        write_header();

        // 2. Install logged operation
        for (int i = 0; i < log.num_blocks; i++) {
            Block *b = bp[i];
            ASSERT(b->acquired);
            cache_sync(NULL, b);
            b->pinned = false;
            b->valid = true;
            cache_release(b);
        }

        // 3. Checkpointed. Wake up all op and detach ctx->node from oplist
        auto nd = op_head.next;
        while(nd != &op_head)
        {
            auto c = container_of(nd, OpContext, node); 
            post_sem(&c->ok);
            _detach_from_list(nd);
            nd = op_head.next;
        }

        // 4. Reset op_head and log and header
        remaining_log_num = MIN(sblock->num_log_blocks, LOG_MAX_SIZE);
        if (_query_sem(&op_available) <= 0) {
            post_sem(&op_available);
        }
        log.num_blocks = 0;
        memset(log.bno, 0, sizeof(log.bno));
        memset(&header, 0, sizeof(header));
        write_header();
        _release_spinlock(&log.lock);

        _acquire_spinlock(&op_head_lock);
        _detach_from_list(&op_head);
        _release_spinlock(&op_head_lock);
    }
    _release_spinlock(&op_num_lock);

    unalertable_wait_sem(&ctx->ok);
    // Clean up per-op resources
    memset(ctx, 0, sizeof(ctx));
}

// see `cache.h`.
// hint: you can use `cache_acquire`/`cache_sync` to read/write blocks.
static usize cache_alloc(OpContext* ctx) {
    Block* bm_block = cache_acquire(bm_bno);
    Bitmap(bm, BIT_PER_BLOCK);
    memcpy(bm, bm_block->data, BLOCK_SIZE);
    int i;
    for (i = bm_bno; i < sblock->num_blocks; i++) {
        bool res = bitmap_get(&bm, i);
        if (!res) {
            bitmap_set(&bm, i);
            memcpy(bm_block->data, bm, BLOCK_SIZE);
            cache_sync(ctx, bm_block);
            break;
        }
    }
    cache_release(bm_block);
    ASSERT(i < sblock->num_blocks);

    // init the block with zero
    Block *b = cache_acquire(i);
    memset(b->data, 0, sizeof(b->data));
    cache_sync(NULL, b);
    cache_release(b);
    return i;
}

// see `cache.h`.
// hint: you can use `cache_acquire`/`cache_sync` to read/write blocks.
static void cache_free(OpContext* ctx, usize block_no) {
    Block* bm_block = cache_acquire(bm_bno);
    Bitmap(bm, BIT_PER_BLOCK);
    memcpy(bm, bm_block->data, BLOCK_SIZE);
    bitmap_clear(&bm, block_no);
    memcpy(bm_block->data, bm, BLOCK_SIZE);
    cache_sync(ctx, bm_block);
    cache_release(bm_block);
}

BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};
