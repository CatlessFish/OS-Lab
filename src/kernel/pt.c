#include <kernel/pt.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <aarch64/intrinsic.h>

PTEntriesPtr get_pte(struct pgdir* pgdir, u64 va, bool alloc)
{
    // TODO
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or return NULL if false.
    // THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY PTE.
    PTEntriesPtr pt0, pt1, pt2, pt3, p;
    ASSERT(P2K(0) == 0); // emmm

    pt0 = pgdir->pt;
    if (pt0 == NULL) {
        if (!alloc) return NULL;
        pt0 = kalloc_page();
        pt1 = kalloc_page();
        pt2 = kalloc_page();
        pt3 = kalloc_page();
        p = kalloc_page();
        ASSERT(pt0 && pt1 && pt2 && pt3 && p);
        pgdir->pt = pt0;
        pt0[VA_PART0(va)] = K2P(pt1) | PTE_TABLE;
        pt1[VA_PART1(va)] = K2P(pt2) | PTE_TABLE;
        pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;
        pt3[VA_PART3(va)] = K2P(p) | PTE_TABLE;
        return pt3[VA_PART3(va)];
    }

    pt1 = P2K(PTE_ADDRESS(pt0[VA_PART0(va)]));
    if (pt1 == NULL) {
        if (!alloc) return NULL;
        pt1 = kalloc_page();
        pt2 = kalloc_page();
        pt3 = kalloc_page();
        p = kalloc_page();
        ASSERT(pt1 && pt2 && pt3 && p);
        pt1[VA_PART1(va)] = K2P(pt2) | PTE_TABLE;
        pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;
        pt3[VA_PART3(va)] = K2P(p) | PTE_TABLE;
        return pt3[VA_PART3(va)];
    }

    pt2 = P2K(PTE_ADDRESS(pt1[VA_PART1(va)]));
    if (pt2 == NULL) {
        if (!alloc) return NULL;
        pt2 = kalloc_page();
        pt3 = kalloc_page();
        p = kalloc_page();
        ASSERT(pt2 && pt3 && p);
        pt2[VA_PART2(va)] = K2P(pt3) | PTE_TABLE;
        pt3[VA_PART3(va)] = K2P(p) | PTE_TABLE;
        return pt3[VA_PART3(va)];
    }

    pt3 = P2K(PTE_ADDRESS(pt2[VA_PART2(va)]));
    if (pt3 == NULL) {
        if (!alloc) return NULL;
        pt3 = kalloc_page();
        p = kalloc_page();
        ASSERT(pt3 && p);
        pt3[VA_PART3(va)] = K2P(p) | PTE_TABLE;
    }
    return pt3[VA_PART3(va)];
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
    for (int i0 = 0; i0 < N_PTE_PER_TABLE; i0++) {
        PTEntriesPtr pt1 = pgdir->pt[i0];
        if (pt1 == NULL) continue;
        for(int i1 = 0; i1 < N_PTE_PER_TABLE; i1++) {
            PTEntriesPtr pt2 = P2K(PTE_ADDRESS(pt1[i1]));
            if (pt2 == NULL) continue;
            for(int i2 = 0; i2 < N_PTE_PER_TABLE; i2++) {
                PTEntriesPtr pt3 = P2K(PTE_ADDRESS(pt2[i2]));
                if (pt3 == NULL) continue;
                kfree_page(P2K(PTE_ADDRESS((u64)pt3)));
            }
            kfree_page(P2K(PTE_ADDRESS((u64)pt2)));
        }
        kfree_page(P2K(PTE_ADDRESS((u64)pt1)));
    }
    kfree_page(pgdir->pt);
}

void attach_pgdir(struct pgdir* pgdir)
{
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}



