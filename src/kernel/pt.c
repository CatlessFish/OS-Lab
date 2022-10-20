#include <kernel/pt.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <common/string.h>
#include <aarch64/intrinsic.h>

// #define DEBUG_LOG_VA_PART
// #define DEBUG_LOG_FREEPAGECOUNT
// #define DEBUG_LOG_ALLOCPAGECOUNT

#ifdef DEBUG_LOG_ALLOCPAGECOUNT
int a0 = 0, a1 = 0, a2 = 0, a3 = 0;
#endif

PTEntriesPtr get_pte(struct pgdir* pgdir, u64 va, bool alloc)
{
    // TODO
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or return NULL if false.
    // THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY PTE.
    PTEntriesPtr pt0, pt1, pt2, pt3;

    #ifdef DEBUG_LOG_VA_PART
    printk("Part0: %llx\tPart1: %llx\tPart2: %llx\tPart3: %llx\n", VA_PART0(va), VA_PART1(va), VA_PART2(va), VA_PART3(va));
    #endif

    pt0 = pgdir->pt;
    if (pt0 == NULL) {
        if (!alloc) return NULL;
        pt0 = kalloc_page();
        pt1 = kalloc_page();
        pt2 = kalloc_page();
        pt3 = kalloc_page();
        ASSERT(pt0 && pt1 && pt2 && pt3);
        pgdir->pt = pt0;
        pt0[VA_PART0(va)] = K2P(pt1) | PTE_TABLE;
        pt1[VA_PART1(va)] = K2P(pt2) | PTE_TABLE;
        pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;

        #ifdef DEBUG_LOG_ALLOCPAGECOUNT
        a0++, a1++, a2++, a3++;
        #endif
        return &pt3[VA_PART3(va)];
    }

    pt1 = (PTEntriesPtr) P2K(PTE_ADDRESS(pt0[VA_PART0(va)]));
    if (K2P(pt1) == NULL) {
        if (!alloc) return NULL;
        pt1 = kalloc_page();
        pt2 = kalloc_page();
        pt3 = kalloc_page();
        ASSERT(pt1 && pt2 && pt3);
        pt0[VA_PART0(va)] = K2P(pt1) | PTE_TABLE;
        pt1[VA_PART1(va)] = K2P(pt2) | PTE_TABLE;
        pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;

        #ifdef DEBUG_LOG_ALLOCPAGECOUNT
        a1++, a2++, a3++;
        #endif
        return &pt3[VA_PART3(va)];
    }

    pt2 = (PTEntriesPtr) P2K(PTE_ADDRESS(pt1[VA_PART1(va)]));
    if (K2P(pt2) == NULL) {
        if (!alloc) return NULL;
        pt2 = kalloc_page();
        pt3 = kalloc_page();
        ASSERT(pt2 && pt3);
        pt1[VA_PART1(va)] = K2P(pt2) | PTE_TABLE;
        pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;

        #ifdef DEBUG_LOG_ALLOCPAGECOUNT
        a2++, a3++;
        #endif
        return &pt3[VA_PART3(va)];
    }

    pt3 = (PTEntriesPtr) P2K(PTE_ADDRESS(pt2[VA_PART2(va)]));
    if (K2P(pt3) == NULL) {
        if (!alloc) return NULL;
        pt3 = kalloc_page();
        ASSERT(pt3);
        pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;

        #ifdef DEBUG_LOG_ALLOCPAGECOUNT
        a3++;
        #endif
    }
    return &pt3[VA_PART3(va)];
}

void init_pgdir(struct pgdir* pgdir)
{
    pgdir->pt = NULL;
}

void free_pgdir(struct pgdir* pgdir)
{
    // TODO
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
    if (pgdir->pt == NULL) return;
    int f0 = 0, f1 = 0, f2 = 0, f3 = 0;
    for (int i0 = 0; i0 < N_PTE_PER_TABLE; i0++) {
        PTEntriesPtr pt1 = (PTEntriesPtr) P2K(PTE_ADDRESS(pgdir->pt[i0]));
        if (K2P(pt1) == NULL) continue;
        for(int i1 = 0; i1 < N_PTE_PER_TABLE; i1++) {
            PTEntriesPtr pt2 = (PTEntriesPtr) P2K(PTE_ADDRESS(pt1[i1]));
            if (K2P(pt2) == NULL) continue;
            for(int i2 = 0; i2 < N_PTE_PER_TABLE; i2++) {
                PTEntriesPtr pt3 = (PTEntriesPtr) P2K(PTE_ADDRESS(pt2[i2]));
                if (K2P(pt3) == NULL) continue;
                kfree_page((void*)P2K(PTE_ADDRESS((u64)pt3)));
                f3++;
            }
            kfree_page((void*)P2K(PTE_ADDRESS((u64)pt2)));
            f2++;
        }
        kfree_page((void*)P2K(PTE_ADDRESS((u64)pt1)));
        f1++;
    }
    kfree_page((void*)pgdir->pt);
    f0++;

    #ifdef DEBUG_LOG_FREEPAGECOUNT
    printk("Free count:\nL0: %d\tL1: %d\tL2: %d\tL3: %d\n", f0, f1, f2, f3);
    #endif
}

void attach_pgdir(struct pgdir* pgdir)
{
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}



