/*
 * Port of kvm-unit-tests x86/access.c to 32-bit. The upstream test is
 * 64-bit-only: Makefile.i386 has "$(TEST_DIR)/access.flat" commented out
 * ("These tests from Makefile.x86_64 don't compile") because it drives
 * 4-level page tables, NX, PKU and SMEP.
 *
 * This is the page-rights table test: for every legal combination of the
 * PTE/PDE bits, the CPU feature bits and the access kind, the expected
 * fault / error code / PTE / PDE update comes from the same model
 * (ac_test_permissions + ac_emulate_access), and the access is actually
 * performed at the given ring. It is the test that covers
 * "a ring-3 write to a user-readable, read-only page faults whatever
 * CR0.WP says" -- the cell the stage-4 片 15 fix restored, and the reason
 * test/x86/pm's t18 (ring-3 write to a U=0 page) and t16 (ring-0 write to
 * a W=0 page, CR0.WP=1) both missed it.
 *
 * Port notes (the removed axes are registered as AGENTS.md D31):
 *   - Removed: NX (PTE/PDE bit 63 + EFER.NXE) and PTE/PDE bit 51 -- this
 *     machine has neither 64-bit nor PAE paging; PKU (AD/WD/PKEY) and SMEP
 *     -- CPUID reports neither and CR4 has no such bits.
 *   - Removed: AC_ACCESS_TWICE and the three KVM-MMU ac_test_cases
 *     (corrupt_hugepage_triger, check_pfec_on_prefetch_pte,
 *     check_smep_andnot_wp): they test the KVM MMU's own bookkeeping, not
 *     the CPU. check_large_pte_dirty_for_nowp is KEPT -- it is the CR0.WP=0
 *     read-only large-page write, i.e. the 4MB leaf branch.
 *   - The accessed/dirty axes are fixed set: the A/D *update* rule (a read
 *     sets A, a write sets D) is covered by test/x86/pm t17.
 *   - The walk is two levels (PDE, PTE); PSE stays an axis so the 4MB leaf
 *     is covered too.
 *   - The flag space is bounded to the axes a 32-bit non-PAE CPU has --
 *     11 bits, 2048 combinations. Upstream enumerates 27 bits and takes
 *     minutes even natively; cemu interprets at ~1.4 MIPS (200M
 *     instructions in 2m26s), so the full space is not runnable here.
 *   - Addresses: the test's virtual address is 0x40000000 (an unused PDE
 *     slot), the target page is at 8MB and the page-table pool at 16MB --
 *     all inside cemu's default 32MB RAM. Upstream's 0x123400000000 base,
 *     32MB target and 33MB pool do not exist on a 32-bit machine.
 *   - The access trampoline is the upstream one with 32-bit registers
 *     (%esi/%esp/%edx, 32-bit iret/push/pop); the page-fault handler pops
 *     the error code into %ebx, redirects the saved EIP at %esi and returns
 *     fault=1 in %eax, exactly as upstream.
 *
 * Everything else -- the oracle, the per-case check, the report texts and
 * the summary line -- is the upstream code.
 */

#include "libcflat.h"
#include "desc.h"
#include "processor.h"
#include "asm/page.h"
#include "vm.h"

#define CR0_WP_MASK (1UL << 16)  /* SDM vol.3 2.5 */

#define PT_BASE_ADDR_MASK PAGE_MASK
#define PT_PSE_BASE_ADDR_MASK (PAGE_MASK & ~(1UL << 21))

#define PFERR_PRESENT_MASK (1U << 0)
#define PFERR_WRITE_MASK (1U << 1)
#define PFERR_USER_MASK (1U << 2)

/*
 * The upstream macro indexes a 4-level walk (each level 9 bits). This
 * machine walks two levels of 10 bits: the PDE covers 4MB (2^22) and the
 * PTE covers 4KB (2^12), so the shifts are 22 and 12, not 21 and 12.
 */
#define PT_INDEX(address, level) \
    ((((level) == 2 ? (address) >> 22 : (address) >> 12)) & 1023)

/* The test's own mapping: an unused PDE slot well above the image. */
#define AC_TEST_VIRT 0x40000000UL
/* The target page and the page-table pool, both inside the default 32MB. */
#define AC_TEST_PHYS 0x00800000UL
#define AC_POOL_BASE 0x01000000UL
#define AC_POOL_SIZE 0x00400000UL

/*
 * page table access check tests
 */

enum {
    AC_PTE_PRESENT_BIT,
    AC_PTE_WRITABLE_BIT,
    AC_PTE_USER_BIT,

    AC_PDE_PRESENT_BIT,
    AC_PDE_WRITABLE_BIT,
    AC_PDE_USER_BIT,
    AC_PDE_PSE_BIT,

    AC_ACCESS_USER_BIT,
    AC_ACCESS_WRITE_BIT,
    AC_ACCESS_FETCH_BIT,

    AC_CPU_CR0_WP_BIT,

    NR_AC_FLAGS
};

#define AC_PTE_PRESENT_MASK (1 << AC_PTE_PRESENT_BIT)
#define AC_PTE_WRITABLE_MASK (1 << AC_PTE_WRITABLE_BIT)
#define AC_PTE_USER_MASK (1 << AC_PTE_USER_BIT)

#define AC_PDE_PRESENT_MASK (1 << AC_PDE_PRESENT_BIT)
#define AC_PDE_WRITABLE_MASK (1 << AC_PDE_WRITABLE_BIT)
#define AC_PDE_USER_MASK (1 << AC_PDE_USER_BIT)
#define AC_PDE_PSE_MASK (1 << AC_PDE_PSE_BIT)

#define AC_ACCESS_USER_MASK (1 << AC_ACCESS_USER_BIT)
#define AC_ACCESS_WRITE_MASK (1 << AC_ACCESS_WRITE_BIT)
#define AC_ACCESS_FETCH_MASK (1 << AC_ACCESS_FETCH_BIT)

#define AC_CPU_CR0_WP_MASK (1 << AC_CPU_CR0_WP_BIT)

typedef unsigned long pt_element_t;

typedef struct {
    unsigned long flags;
    void *virt;
    unsigned long phys;
    pt_element_t *ptep;
    pt_element_t *pdep;
    pt_element_t expected_pte;
    pt_element_t expected_pde;
    int expected_fault;
    unsigned expected_error;
    pt_element_t ignore_pde;
} ac_test_t;

typedef struct {
    unsigned long pt_pool;
    unsigned long pt_pool_size;
    unsigned long pt_pool_current;
} ac_pool_t;

static void set_cr0_wp(int wp)
{
    unsigned long cr0 = read_cr0();

    if (wp)
        cr0 |= CR0_WP_MASK;
    else
        cr0 &= ~CR0_WP_MASK;
    write_cr0(cr0);
}

static void ac_env_int(ac_pool_t *pool)
{
    extern char page_fault, kernel_entry;

    set_idt_entry(14, &page_fault, 0);
    set_idt_entry(0x20, &kernel_entry, 3);

    pool->pt_pool = AC_POOL_BASE;
    pool->pt_pool_size = AC_POOL_SIZE;
    pool->pt_pool_current = 0;
}

static void ac_test_init(ac_test_t *at, void *virt)
{
    set_cr0_wp(1);
    at->flags = 0;
    at->virt = virt;
    at->phys = AC_TEST_PHYS;
}

int ac_test_bump_one(ac_test_t *at)
{
    at->flags = (at->flags + 1) & ((1 << NR_AC_FLAGS) - 1);
    return at->flags != 0;
}

#define F(x) ((flags & x##_MASK) != 0)

static _Bool ac_test_legal(ac_test_t *at)
{
    int flags = at->flags;

    if (F(AC_ACCESS_FETCH) && F(AC_ACCESS_WRITE))
        return false;

    return true;
}

int ac_test_bump(ac_test_t *at)
{
    int ret;

    ret = ac_test_bump_one(at);
    while (ret && !ac_test_legal(at))
        ret = ac_test_bump_one(at);
    return ret;
}

static pt_element_t ac_test_alloc_pt(ac_pool_t *pool)
{
    pt_element_t ret = pool->pt_pool + pool->pt_pool_current;

    pool->pt_pool_current += PAGE_SIZE;
    return ret;
}

static _Bool ac_test_enough_room(ac_pool_t *pool)
{
    return pool->pt_pool_current + 4 * PAGE_SIZE <= pool->pt_pool_size;
}

static void ac_test_reset_pt_pool(ac_pool_t *pool)
{
    pool->pt_pool_current = 0;
}

static pt_element_t ac_test_permissions(ac_test_t *at, unsigned flags,
                                        bool writable, bool user,
                                        bool executable)
{
    bool kwritable = !F(AC_CPU_CR0_WP) && !F(AC_ACCESS_USER);
    pt_element_t expected = 0;

    if (F(AC_ACCESS_USER) && !user)
        at->expected_fault = 1;

    if (F(AC_ACCESS_WRITE) && !writable && !kwritable)
        at->expected_fault = 1;

    if (F(AC_ACCESS_FETCH) && !executable)
        at->expected_fault = 1;

    if (!at->expected_fault) {
        expected |= PT_ACCESSED_MASK;
        if (F(AC_ACCESS_WRITE))
            expected |= PT_DIRTY_MASK;
    }

    return expected;
}

static void ac_emulate_access(ac_test_t *at, unsigned flags)
{
    bool pde_valid, pte_valid;
    bool user, writable, executable;

    if (F(AC_ACCESS_USER))
        at->expected_error |= PFERR_USER_MASK;

    if (F(AC_ACCESS_WRITE))
        at->expected_error |= PFERR_WRITE_MASK;

    pde_valid = F(AC_PDE_PRESENT);

    if (!pde_valid) {
        at->expected_fault = 1;
        at->expected_error &= ~PFERR_PRESENT_MASK;
        goto fault;
    }

    writable = F(AC_PDE_WRITABLE);
    user = F(AC_PDE_USER);
    executable = true;  /* no NX on this machine */

    if (F(AC_PDE_PSE)) {
        at->expected_pde |= ac_test_permissions(at, flags, writable, user,
                                                executable);
        goto no_pte;
    }

    at->expected_pde |= PT_ACCESSED_MASK;

    pte_valid = F(AC_PTE_PRESENT);

    if (!pte_valid) {
        at->expected_fault = 1;
        at->expected_error &= ~PFERR_PRESENT_MASK;
        goto fault;
    }

    writable &= F(AC_PTE_WRITABLE);
    user &= F(AC_PTE_USER);

    at->expected_pte |= ac_test_permissions(at, flags, writable, user,
                                            executable);

no_pte:
fault:
    if (!at->expected_fault)
        at->ignore_pde = 0;
}

static void ac_set_expected_status(ac_test_t *at)
{
    invlpg(at->virt);

    if (at->ptep)
        at->expected_pte = *at->ptep;
    at->expected_pde = *at->pdep;
    at->ignore_pde = 0;
    at->expected_fault = 0;
    at->expected_error = PFERR_PRESENT_MASK;

    ac_emulate_access(at, at->flags);
}

static void ac_test_setup_pte(ac_test_t *at, ac_pool_t *pool)
{
    unsigned long root = read_cr3();
    int flags = at->flags;

    if (!ac_test_enough_room(pool))
        ac_test_reset_pt_pool(pool);

    at->ptep = 0;
    for (int i = 2; i >= 1 && (i >= 2 || !F(AC_PDE_PSE)); --i) {
        pt_element_t *vroot = phys_to_virt(root & PT_BASE_ADDR_MASK);
        unsigned index = PT_INDEX((unsigned long)at->virt, i);
        pt_element_t pte = 0;

        switch (i) {
        case 2:
            if (!F(AC_PDE_PSE)) {
                pte = ac_test_alloc_pt(pool);
            } else {
                pte = at->phys & PT_PSE_BASE_ADDR_MASK;
                pte |= PT_PAGE_SIZE_MASK;
            }
            if (F(AC_PDE_PRESENT))
                pte |= PT_PRESENT_MASK;
            if (F(AC_PDE_WRITABLE))
                pte |= PT_WRITABLE_MASK;
            if (F(AC_PDE_USER))
                pte |= PT_USER_MASK;
            pte |= PT_ACCESSED_MASK | PT_DIRTY_MASK;
            at->pdep = &vroot[index];
            break;
        case 1:
            pte = at->phys & PT_BASE_ADDR_MASK;
            if (F(AC_PTE_PRESENT))
                pte |= PT_PRESENT_MASK;
            if (F(AC_PTE_WRITABLE))
                pte |= PT_WRITABLE_MASK;
            if (F(AC_PTE_USER))
                pte |= PT_USER_MASK;
            pte |= PT_ACCESSED_MASK | PT_DIRTY_MASK;
            at->ptep = &vroot[index];
            break;
        }
        vroot[index] = pte;
        root = vroot[index];
    }
    ac_set_expected_status(at);
}

static const char *const kFlagNames[NR_AC_FLAGS] = {
    "pte.present", "pte.writable", "pte.user",
    "pde.present", "pde.writable", "pde.user", "pde.pse",
    "access.user", "access.write", "access.fetch",
    "cpu.cr0_wp",
};

// One line per failure with the flags inline, and a per-flag histogram in the
// summary: with hundreds of failures it is the *pattern* across them that
// localizes the axis, and neither a multi-line dump per case nor a mapping
// dump helps there (it also costs more instructions than the test itself).
static void ac_test_check(ac_test_t *at, _Bool *success_ret, _Bool cond,
                          const char *fmt, ...)
{
    va_list ap;
    char buf[500];

    if (!*success_ret)
        return;

    if (!cond)
        return;

    *success_ret = false;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    printf("FAIL [");
    for (int i = 0; i < NR_AC_FLAGS; i++)
        if (at->flags & (1UL << i))
            printf("%s ", kFlagNames[i]);
    printf("]: %s\n", buf);
}

static int pt_match(pt_element_t pte1, pt_element_t pte2, pt_element_t ignore)
{
    pte1 &= ~ignore;
    pte2 &= ~ignore;
    return pte1 == pte2;
}

static int ac_test_do_access(ac_test_t *at)
{
    static unsigned unique = 42;
    int fault = 0;
    unsigned e;
    static unsigned char user_stack[4096];
    unsigned long esp;
    _Bool success = true;
    int flags = at->flags;

    ++unique;
    if (!(unique & 65535))
        puts(".");

    *((unsigned char *)at->phys) = 0xc3; /* ret */

    unsigned r = unique;
    /* The 32-bit register file has no room for these as register operands
     * (eax/ebx/edx/esi are pinned by the trampoline), so they go in memory. */
    int wflag = F(AC_ACCESS_WRITE);
    int uflag = F(AC_ACCESS_USER);
    int fflag = F(AC_ACCESS_FETCH);
    void *ustack = user_stack + sizeof user_stack;

    set_cr0_wp(F(AC_CPU_CR0_WP));

    asm volatile("mov $fixed1, %%esi \n\t"
                 "mov %%esp, %%edx \n\t"
                 "cmpl $0, %[user] \n\t"
                 "jz do_access \n\t"
                 "push %%eax; mov %[user_ds], %%ax; mov %%ax, %%ds; pop %%eax \n\t"
                 "pushl %[user_ds] \n\t"
                 "pushl %[user_stack_top] \n\t"
                 "pushfl \n\t"
                 "pushl %[user_cs] \n\t"
                 "pushl $do_access \n\t"
                 "iret \n"
                 "do_access: \n\t"
                 "cmpl $0, %[fetch] \n\t"
                 "jnz 2f \n\t"
                 "cmpl $0, %[write] \n\t"
                 "jnz 1f \n\t"
                 "mov (%[addr]), %[reg] \n\t"
                 "jmp done \n\t"
                 "1: mov %[reg], (%[addr]) \n\t"
                 "jmp done \n\t"
                 "2: call *%[addr] \n\t"
                 "done: \n"
                 "fixed1: \n"
                 "int %[kernel_entry_vector] \n\t"
                 "back_to_kernel:"
                 : [reg] "+r"(r), "+a"(fault), "=b"(e), "=&d"(esp)
                 : [addr] "r"(at->virt),
                   [write] "m"(wflag),
                   [user] "m"(uflag),
                   [fetch] "m"(fflag),
                   [user_ds] "i"(USER_DS),
                   [user_cs] "i"(USER_CS),
                   [user_stack_top] "m"(ustack),
                   [kernel_entry_vector] "i"(0x20)
                 : "esi");

    asm volatile(".section .text.pf \n\t"
                 "page_fault: \n\t"
                 "pop %ebx \n\t"
                 "mov %esi, (%esp) \n\t"
                 "movl $1, %eax \n\t"
                 "iret \n\t"
                 ".section .text");

    asm volatile(".section .text.entry \n\t"
                 "kernel_entry: \n\t"
                 "mov %edx, %esp \n\t"
                 "jmp back_to_kernel \n\t"
                 ".section .text");

    ac_test_check(at, &success, fault && !at->expected_fault,
                  "unexpected fault");
    ac_test_check(at, &success, !fault && at->expected_fault,
                  "unexpected access");
    ac_test_check(at, &success, fault && e != at->expected_error,
                  "error code %x expected %x", e, at->expected_error);
    /*
     * The PSE cases have no PTE: at->ptep is NULL and the leaf is the PDE, so
     * there is no PTE to compare. The read has to happen under the guard --
     * upstream writes the condition as
     *     at->ptep && *at->ptep != at->expected_pte, "pte %x ...", *at->ptep
     * but there *at->ptep is dereferenced unconditionally as a variadic
     * argument, and since a NULL dereference is UB the optimizer is entitled
     * to drop the guard and hoist the load. GCC -O2 does exactly that: every
     * PSE case then read linear address 0 (the real-mode IVT) and compared the
     * 0x80cf0 found there against the previous case's expected_pte, which
     * turned all 768 of them into failures with no other check ever firing.
     */
    pt_element_t actual_pte = at->ptep ? *at->ptep : at->expected_pte;

    ac_test_check(at, &success, at->ptep && actual_pte != at->expected_pte,
                  "pte %lx expected %lx", actual_pte, at->expected_pte);
    ac_test_check(at, &success,
                  !pt_match(*at->pdep, at->expected_pde, at->ignore_pde),
                  "pde %lx expected %lx", *at->pdep, at->expected_pde);

    return success;
}

/*
 * A CR0.WP=0 supervisor write through a read-only 4MB leaf must succeed and
 * leave D set (the 4MB leaf branch of page_translate_as).
 */
static int check_large_pte_dirty_for_nowp(ac_pool_t *pool)
{
    ac_test_t at1, at2;

    ac_test_init(&at1, (void *)(AC_TEST_VIRT + 0x1000));
    ac_test_init(&at2, (void *)(AC_TEST_VIRT + 0x2000));

    at2.flags = AC_PDE_PRESENT_MASK | AC_PDE_PSE_MASK;
    ac_test_setup_pte(&at2, pool);
    if (!ac_test_do_access(&at2)) {
        printf("%s: read on the first mapping fail.\n", __FUNCTION__);
        goto err;
    }

    at1.flags = at2.flags | AC_ACCESS_WRITE_MASK;
    ac_test_setup_pte(&at1, pool);
    if (!ac_test_do_access(&at1)) {
        printf("%s: write on the second mapping fail.\n", __FUNCTION__);
        goto err;
    }

    at2.flags |= AC_ACCESS_WRITE_MASK;
    ac_set_expected_status(&at2);
    if (!ac_test_do_access(&at2)) {
        printf("%s: write on the first mapping fail.\n", __FUNCTION__);
        goto err;
    }

    return 1;

err:
    return 0;
}

static int ac_test_exec(ac_test_t *at, ac_pool_t *pool)
{
    ac_test_setup_pte(at, pool);
    return ac_test_do_access(at);
}

int ac_test_run(void)
{
    ac_test_t at;
    ac_pool_t pool;
    int tests, successes;
    int fail_with[NR_AC_FLAGS];
    int fails = 0;
    int i;

    printf("run\n");
    tests = successes = 0;
    for (i = 0; i < NR_AC_FLAGS; i++)
        fail_with[i] = 0;

    ac_env_int(&pool);
    ac_test_init(&at, (void *)AC_TEST_VIRT);
    do {
        ++tests;
        if (ac_test_exec(&at, &pool)) {
            ++successes;
        } else {
            ++fails;
            for (i = 0; i < NR_AC_FLAGS; i++)
                if (at.flags & (1UL << i))
                    fail_with[i]++;
        }
    } while (ac_test_bump(&at));

    ++tests;
    successes += check_large_pte_dirty_for_nowp(&pool);

    printf("\n%d tests, %d failures\n", tests, tests - successes);
    if (fails) {
        printf("failures by flag (out of %d):\n", fails);
        for (i = 0; i < NR_AC_FLAGS; i++)
            printf("  %-16s %d\n", kFlagNames[i], fail_with[i]);
    }

    return successes == tests;
}

int main(void)
{
    setup_idt();
    printf("starting test\n\n");
    return ac_test_run() ? 0 : 1;
}
