/*
  张津硕  challenge1 实现buddy system
*/

#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <buddy_system_pmm.h>
typedef int bool;
#define true 1
#define false 0



/* 如果头文件未定义 MAX_BUDDY_ORDER（保护），则默认支持 2^15 页 ~ 128MB */
#ifndef MAX_BUDDY_ORDER
#define MAX_BUDDY_ORDER 15
#endif

/* 内部数据结构：每个 order 一个链表 */
static list_entry_t buddy_array[MAX_BUDDY_ORDER + 1];
static unsigned int max_order = 0;   /* 当前系统中出现的最大阶（初始化后设置） */
static size_t nr_free = 0;           /* 当前空闲总页数 */

/* 辅助宏（使用全局 pages/npage） */
extern struct Page *pages;
extern size_t npage;
#define page2idx(p) ((size_t)((p) - pages))
#define idx2page(i) (pages + (i))

/* 判断是否 2 的幂 */
static inline int is_pow2(size_t x) {
    return x && ((x & (x - 1)) == 0);
}

/* 向上取最近 2 的幂（至少 1），用于将请求页数调整到 2^k */
static size_t round_up_pow2(size_t x) {
    if (x <= 1) return 1;
    size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

/* 向下取不超过 x 的最大 2 的幂（至少 1） */
static size_t round_down_pow2(size_t x) {
    size_t p = 1;
    while ((p << 1) <= x) p <<= 1;
    return p;
}

/* 给定 pow2 (比如 8) 返回其 order(3)；假设 x 为 2^k */
static unsigned int order_of_pow2(size_t x) {
    unsigned int o = 0;
    while (x > 1) { x >>= 1; o++; }
    return o;
}

/* 将 [addr, addr + n) 分割成尽可能大的对齐块并加入对应链表:
   addr: 页号（page2idx(base)）， n: 页数
   规则：对于当前位置 addr，尽量选择最大 k 使得 (1<<k) <= n && (addr % (1<<k) == 0)
*/
static void insert_range_as_aligned_blocks(size_t addr, size_t n) {
    while (n > 0) {
        /* 找到最大的 k 满足块大小 <= n 且对齐 */
        unsigned int k = 0;
        /* 先从最大能容纳的幂开始 */
        size_t max_block = round_down_pow2(n);
        /* 但是需要满足 addr 对齐约束：addr % (1<<k) == 0 */
        /* 我们从 max possible downwards 找第一个对齐的 */
        size_t block = max_block;
        while (block > 0) {
            unsigned int ord = order_of_pow2(block);
            if ((addr & ((1u << ord) - 1)) == 0) {
                k = ord;
                break;
            }
            block >>= 1;
        }
        /* 插入块 [addr, addr + (1<<k)) */
        struct Page *page = idx2page(addr);
        page->property = k;
        SetPageProperty(page);
        list_add(&buddy_array[k], &page->page_link);
        if (k > max_order) max_order = k;
        nr_free += (1u << k);
        addr += (1u << k);
        n -= (1u << k);
    }
}

/* split：把 buddy_array[order] 的第一个块分裂成两个 order-1 的块并放回链表 */
static void buddy_system_split(unsigned int order) {
    assert(order > 0 && order <= MAX_BUDDY_ORDER);
    assert(!list_empty(&buddy_array[order]));

    list_entry_t *le = list_next(&buddy_array[order]);
    struct Page *block = le2page(le, page_link);
    list_del(le);

    unsigned int sub_order = order - 1;
    struct Page *left = block;
    struct Page *right = block + (1u << sub_order);

    left->property = sub_order;
    right->property = sub_order;
    SetPageProperty(left);
    SetPageProperty(right);

    /* 将两个子块放回 sub_order 链表（先插 left 再 right，顺序并不关键） */
    list_add(&buddy_array[sub_order], &left->page_link);
    list_add(&buddy_array[sub_order], &right->page_link);
}

/* 计算某页块在给定 order 下的伙伴页（基于页号的异或） */
static struct Page *get_buddy(struct Page *block, unsigned int order) {
    size_t idx = page2idx(block);
    size_t buddy_idx = idx ^ (1u << order);
    /* 边界检查 */
    if (buddy_idx >= npage) return NULL;
    return idx2page(buddy_idx);
}

/* 初始化：清空所有链表与计数 */
static void buddy_system_init(void) {
    for (int i = 0; i <= MAX_BUDDY_ORDER; i++) {
        list_init(&buddy_array[i]);
    }
    max_order = 0;
    nr_free = 0;
}

/* 初始化内存映射：把 base..base+n-1 页划分为尽可能大的对齐块并插入链表 */
static void buddy_system_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    /* 清理每页标志（保持与系统约定一致） */
    for (struct Page *p = base; p != base + n; p++) {
        /* 这里参考常见实现，要求 p 已经预留（PageReserved）或按你的内核约定 */
        assert(PageReserved(p));
        p->flags = 0;
        set_page_ref(p, 0);
        ClearPageProperty(p);
    }

    size_t addr = page2idx(base);
    insert_range_as_aligned_blocks(addr, n);
}

/* 分配 pages：把请求上调为 2^k，然后找合适块并分裂直至到达 k */
static struct Page *buddy_system_alloc_pages(size_t requested_pages) {
    assert(requested_pages > 0);
    if (requested_pages > nr_free) return NULL;

    size_t size = round_up_pow2(requested_pages);
    unsigned int order = order_of_pow2(size);
    if (order > MAX_BUDDY_ORDER) return NULL;

    /* 找到 >= order 的第一个非空链表 */
    unsigned int i = order;
    while (i <= MAX_BUDDY_ORDER && list_empty(&buddy_array[i])) i++;
    if (i > MAX_BUDDY_ORDER) return NULL;

    /* 自上而下分裂直到 order */
    while (i > order) {
        /* split the first block in buddy_array[i] into two of order i-1 */
        buddy_system_split(i);
        i--;
    }

    /* 现在 buddy_array[order] 非空，拿出第一个块 */
    list_entry_t *le = list_next(&buddy_array[order]);
    struct Page *page = le2page(le, page_link);
    list_del(le);
    ClearPageProperty(page);

    /* 更新总空闲页 */
    nr_free -= (1u << order);
    return page;
}

/* 释放 pages：base 必须是块头页（property 指示阶）或者 n 给出释放页数与块一致 */
static void buddy_system_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    /* 将 n 调整为块大小（必须是 2^k），并计算order */
    size_t blk_size = round_up_pow2(n);
    unsigned int order = order_of_pow2(blk_size);
    assert((1u << order) == blk_size);

    /* 将 base 标记为该 order 的块头并插入 */
    base->property = order;
    SetPageProperty(base);
    list_add(&buddy_array[order], &base->page_link);

    /* 尝试合并 */
    struct Page *left = base;
    while (order < MAX_BUDDY_ORDER) {
        struct Page *buddy = get_buddy(left, order);
        if (buddy == NULL) break;
        /* 只有当伙伴存在且是空闲且同阶时才合并 */
        if (!PageProperty(buddy) || buddy->property != order) break;
        /* 从链表中删除 left 和 buddy（注意可能 buddy 在链表任何位置） */
        list_del(&left->page_link);
        list_del(&buddy->page_link);
        /* 计算合并后新的左侧基址（较小的地址）*/
        if (left > buddy) {
            struct Page *tmp = left;
            left = buddy;
            buddy = tmp;
        }
        /* 新块阶升一 */
        order++;
        left->property = order;
        /* 将新的大块插回对应链表以便可能继续合并 */
        list_add(&buddy_array[order], &left->page_link);
    }

    /* 最终把合并后的 left 标记好 */
    SetPageProperty(left);
    nr_free += blk_size;
    if (order > max_order) max_order = order;
}

/* 统计空闲页 */
static size_t buddy_system_nr_free_pages(void) {
    return nr_free;
}

/* 打印当前结构（保留你原来的 show_buddy_array 风格，） */
static void show_buddy_array(int left, int right) {
    assert(left >= 0 && left <= MAX_BUDDY_ORDER && right >= 0 && right <= MAX_BUDDY_ORDER);
    cprintf("------------------当前空闲的链表数组:------------------\n");
    bool empty_all = true;
    for (int i = left; i <= right; i++) {
        list_entry_t *le = &buddy_array[i];
        if (list_next(le) != le) {
            empty_all = false;
            cprintf("order %2d : ", i);
            list_entry_t *it = list_next(le);
            while (it != le) {
                struct Page *p = le2page(it, page_link);
                cprintf("[%u pages, idx=%u] ", (1u << p->property), (unsigned)page2idx(p));
                cprintf("【地址为%p】\n", p);//输出当前链表元素对应的页的地址
                it = list_next(it);
            }
            cprintf("\n");
        }
    }
    if (empty_all) {
        cprintf("目前无空闲块！！！\n");
    }
    cprintf("------------------显示完成!------------------\n\n\n");
}

/* ------- 你的测试用例（保留并稍微调用 show_buddy_array） ------- */

static void buddy_system_check_easy_alloc_and_free_condition(void) {
    cprintf("CHECK OUR EASY ALLOC CONDITION:\n");
    cprintf("当前总的空闲页的数量为：%u\n", (unsigned)nr_free);
    show_buddy_array(0, MAX_BUDDY_ORDER);
    struct Page *p0, *p1, *p2;
    p0 = p1 = p2 = NULL;

    cprintf("首先,p0请求16页\n");
    p0 = alloc_pages(16);
    show_buddy_array(0, MAX_BUDDY_ORDER);

    cprintf("然后,p1请求4页\n");
    p1 = alloc_pages(4);
    show_buddy_array(0, MAX_BUDDY_ORDER);

    cprintf("最后,p2请求500页\n");
    p2 = alloc_pages(500);
    show_buddy_array(0, MAX_BUDDY_ORDER);

    cprintf("p0 idx=%u\n", (unsigned)page2idx(p0));
    cprintf("p1 idx=%u\n", (unsigned)page2idx(p1));
    cprintf("p2 idx=%u\n", (unsigned)page2idx(p2));

    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(page_ref(p0) == 0 && page_ref(p1) == 0 && page_ref(p2) == 0);
    assert(page2pa(p0) < npage * PGSIZE);
    assert(page2pa(p1) < npage * PGSIZE);
    assert(page2pa(p2) < npage * PGSIZE);

    cprintf("CHECK OUR EASY FREE CONDITION:\n");
    cprintf("释放p0...\n");
    free_pages(p0, 16);
    cprintf("释放p0后,总空闲页数为:%u\n", (unsigned)nr_free);
    show_buddy_array(0, MAX_BUDDY_ORDER);

    cprintf("释放p1...\n");
    free_pages(p1, 4);
    cprintf("释放p1后,总空闲页数为:%u\n", (unsigned)nr_free);
    show_buddy_array(0, MAX_BUDDY_ORDER);

    cprintf("释放p2...\n");
    free_pages(p2, 500);
    cprintf("释放p2后,总空闲页数为:%u\n", (unsigned)nr_free);
    show_buddy_array(0, MAX_BUDDY_ORDER);
}



static void buddy_system_check_min_alloc_and_free_condition(void) {
    struct Page *p3 = alloc_pages(1);
    cprintf("分配p3之后(1页)\n");
    show_buddy_array(0, MAX_BUDDY_ORDER);

    free_pages(p3, 1);
    show_buddy_array(0, MAX_BUDDY_ORDER);
}

static void buddy_system_check_max_alloc_and_free_condition(void) {
    size_t big = 1u << MAX_BUDDY_ORDER;
    if (big > npage) big = round_down_pow2(npage);
    struct Page *p3 = alloc_pages(big);
    cprintf("分配p3之后(%u页)\n", (unsigned)big);
    show_buddy_array(0, MAX_BUDDY_ORDER);

    free_pages(p3, big);
    show_buddy_array(0, MAX_BUDDY_ORDER);
}

static void buddy_system_check(void) {
    cprintf("BEGIN TO TEST OUR BUDDY SYSTEM!\n");
    buddy_system_check_easy_alloc_and_free_condition();
    buddy_system_check_min_alloc_and_free_condition();
    buddy_system_check_max_alloc_and_free_condition();
}

/* 导出 pmm 管理器结构，供 pmm.c 使用 */
const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_system_init,
    .init_memmap = buddy_system_init_memmap,
    .alloc_pages = buddy_system_alloc_pages,
    .free_pages = buddy_system_free_pages,
    .nr_free_pages = buddy_system_nr_free_pages,
    .check = buddy_system_check,
};
