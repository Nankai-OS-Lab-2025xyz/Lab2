// 2313226 肖俊涛
// challenge2 slub算法


#include <pmm.h>
#include <list.h>
#include <string.h>
#include <slub_pmm.h>
#include <stdio.h>
free_area_t free_area_slub;

#define free_list (free_area_slub.free_list)
#define nr_free (free_area_slub.nr_free)
// #define MIN_ALLOC_SIZE 64     // 定义最小分配单位
// #define MAX_ALLOC_SIZE PGSIZE // 定义最大分配单位为一个页的大小

struct SlubBlock
{
    size_t size;            // 小块的大小
    void *page;             // 指向分配的页面
    struct SlubBlock *next; // 指向下一个小块
};

// static struct SlubBlock *slub_small_block_list[MAX_ALLOC_SIZE / MIN_ALLOC_SIZE];
// struct SlubBlock slub_block[MAX_ALLOC_SIZE / MIN_ALLOC_SIZE];
// struct SlubBlock *slubfree[MAX_ALLOC_SIZE / MIN_ALLOC_SIZE];
static struct SlubBlock *slub_small_block_list;
struct SlubBlock slub_block;
static void
slub_init(void)
{
    list_init(&free_list);
    nr_free = 0;
    // for (int i = 0; i < MAX_ALLOC_SIZE / MIN_ALLOC_SIZE; i++)
    // {
    //     slub_small_block_list[i] = NULL;
    //     slub_block[i].size = MIN_ALLOC_SIZE * (i + 1);
    //     slub_block[i].page = NULL;
    //     slub_block[i].next = &slub_block[i];
    //     slubfree[i]=&slub_block[i];
    // }
    slub_small_block_list = NULL;
    slub_block.size = 0;
    slub_block.page = NULL;
    slub_block.next = &slub_block;
}

static void
slub_init_memmap(struct Page *base, size_t n)
{
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p++)
    {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    if (list_empty(&free_list))
    {
        list_add(&free_list, &(base->page_link));
    }
    else
    {
        list_entry_t *le = &free_list;
        while ((le = list_next(le)) != &free_list)
        {
            struct Page *page = le2page(le, page_link);
            if (base < page)
            {
                list_add_before(le, &(base->page_link));
                break;
            }
            else if (list_next(le) == &free_list)
            {
                list_add(le, &(base->page_link));
            }
        }
    }
}
// 初始化小块分配链表
static struct Page *
slub_alloc_pages(size_t n)
{
    assert(n > 0);
    if (n > nr_free)
    {
        return NULL;
    }
    struct Page *page = NULL;
    list_entry_t *le = &free_list;
    size_t min_size = nr_free + 1;
    while ((le = list_next(le)) != &free_list)
    {
        struct Page *p = le2page(le, page_link);
        if (p->property >= n && p->property < min_size)
        {
            page = p;
            min_size = p->property;
        }
    }

    if (page != NULL)
    {
        list_entry_t *prev = list_prev(&(page->page_link));
        list_del(&(page->page_link));
        if (page->property > n)
        {
            struct Page *p = page + n;
            p->property = page->property - n;
            SetPageProperty(p);
            list_add(prev, &(p->page_link));
        }
        nr_free -= n;
        ClearPageProperty(page);
    }
    return page;
}

static void
slub_free_pages(struct Page *base, size_t n)
{
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p++)
    {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;

    if (list_empty(&free_list))
    {
        list_add(&free_list, &(base->page_link));
    }
    else
    {
        list_entry_t *le = &free_list;
        while ((le = list_next(le)) != &free_list)
        {
            struct Page *page = le2page(le, page_link);
            if (base < page)
            {
                list_add_before(le, &(base->page_link));
                break;
            }
            else if (list_next(le) == &free_list)
            {
                list_add(le, &(base->page_link));
            }
        }
    }

    list_entry_t *le = list_prev(&(base->page_link));
    if (le != &free_list)
    {
        p = le2page(le, page_link);
        if (p + p->property == base)
        {
            p->property += base->property;
            ClearPageProperty(base);
            list_del(&(base->page_link));
            base = p;
        }
    }

    le = list_next(&(base->page_link));
    if (le != &free_list)
    {
        p = le2page(le, page_link);
        if (base + base->property == p)
        {
            base->property += p->property;
            ClearPageProperty(p);
            list_del(&(p->page_link));
        }
    }
}

static size_t
slub_nr_free_pages(void)
{
    return nr_free;
}

static void slub_free_small(void *ptr, size_t size)
{
    if (ptr == NULL)
    {
        return;
    }
    struct SlubBlock *block = (struct SlubBlock *)ptr - 1;
    size_t total_size = size * PGSIZE;
    block->size += total_size;
    struct SlubBlock *temp = &slub_block;
    while (temp->next != &slub_block)
    {
        if (temp->size <= block->size)
        {
            struct SlubBlock *next = temp->next;
            if (next == &slub_block || next > block)
            {
                temp->next = block;
                block->next = next;
                return;
            }
            temp = temp->next;
        }
        else
        {
            temp = temp->next;
        }
    }
}

static void *slub_alloc_small(float size)
{
    // int index = get_block_index(size);
    size_t total_size = size * PGSIZE; // 计算总大小
    struct SlubBlock *temp = &slub_block;
    while (temp->next != &slub_block)
    {
        if (temp->size >= total_size)
        {
            struct SlubBlock *block = temp->next;
            temp->next = block->next;
            return (void *)(block + 1);
        }
        else
        {
            temp = temp->next;
        }
    }
    // 没有找到匹配项
    struct Page *page = slub_alloc_pages(1); // 分配一个页
    if (page == NULL)
    {
        return NULL; // 分配失败
    }
    struct SlubBlock *current_block = (struct SlubBlock *)page; // 获取页面指针
    current_block->size = 0;                                    // 设置大小
    slub_free_small((void *)(current_block + 1), 1);
    return (void *)(current_block + 1);
}

static struct Page *slub_alloc(size_t size)
{
    size = size * PGSIZE;
    if (size >= PGSIZE)
    {
        return slub_alloc_pages((size + PGSIZE - 1) / PGSIZE); // 大于一页时，直接分配整页
    }

    // 获取小块
    void *small_block_ptr = slub_alloc_small(size);
    if (small_block_ptr)
    {
        struct SlubBlock *block = (struct SlubBlock *)small_block_ptr - 1;
        return block->page; // 返回关联的页面
    }

    return NULL; // 分配失败
}

static void slub_free(struct Page *ptr, size_t size)
{
    size *= PGSIZE;
    if (size >= PGSIZE)
    {
        slub_free_pages(ptr, (size + PGSIZE - 1) / PGSIZE); // 释放整页
    }
    else
    {
        slub_free_small(ptr, size); // 释放小块
    }
}

static void
basic_check(void)
{
    struct Page *p0, *p1, *p2;
    p0 = p1 = p2 = NULL;
    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(page_ref(p0) == 0 && page_ref(p1) == 0 && page_ref(p2) == 0);

    assert(page2pa(p0) < npage * PGSIZE);
    assert(page2pa(p1) < npage * PGSIZE);
    assert(page2pa(p2) < npage * PGSIZE);

    list_entry_t free_list_store = free_list;
    list_init(&free_list);
    assert(list_empty(&free_list));

    unsigned int nr_free_store = nr_free;
    nr_free = 0;

    assert(alloc_page() == NULL);

    free_page(p0);
    free_page(p1);
    free_page(p2);
    assert(nr_free == 3);

    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    assert(alloc_page() == NULL);

    free_page(p0);
    assert(!list_empty(&free_list));

    struct Page *p;
    assert((p = alloc_page()) == p0);
    assert(alloc_page() == NULL);

    assert(nr_free == 0);
    free_list = free_list_store;
    nr_free = nr_free_store;

    free_page(p);
    free_page(p1);
    free_page(p2);
}
static void
static void slub_check(void)
{
    // —— 可现场改动的测试参数 ——
    const size_t S0 = 64, S1 = 128, S2 = 256;  // 小对象尺寸（字节）
    const int REPEAT = 8;                      // 小对象重复分配次数
    const int BULK   = 16;                     // 批量分配个数
    const size_t NPG = 5;                      // 多页分配页数
    // ————————————————

    cprintf("测试开始\n");

    // 1) 基本小对象分配/释放
    void *p0 = slub_alloc_small(S0);  assert(p0); cprintf("分配%u字节 OK: %p\n", (unsigned)S0, p0);
    void *p1 = slub_alloc_small(S1);  assert(p1); cprintf("分配%u字节 OK: %p\n", (unsigned)S1, p1);
    void *p2 = slub_alloc_small(S2);  assert(p2); cprintf("分配%u字节 OK: %p\n", (unsigned)S2, p2);

    slub_free_small(p0, S0); cprintf("释放%u字节 OK\n", (unsigned)S0);
    slub_free_small(p1, S1); cprintf("释放%u字节 OK\n", (unsigned)S1);
    slub_free_small(p2, S2); cprintf("释放%u字节 OK\n", (unsigned)S2);

    // 2) 重复分配观察地址是否复用（freelist 重用）
    cprintf("重复%u字节分配测试开始（%d 次）\n", (unsigned)S0, REPEAT);
    void *last = NULL;
    for (int i = 0; i < REPEAT; i++) {
        void *q = slub_alloc_small(S0);
        assert(q);
        cprintf("  第%02d次: %p\n", i, q);
        if (last) { /* 看到地址重复更有说服力 */ }
        slub_free_small(q, S0);
        last = q;
    }
    cprintf("重复%u字节分配测试通过\n", (unsigned)S0);

    // 3) 批量分配 + 批量释放，检查一致性
    void *ptr[BULK];
    for (int i = 0; i < BULK; i++) {
        ptr[i] = slub_alloc_small(S0);
        assert(ptr[i]);
    }
    for (int i = 0; i < BULK; i++) {0
        slub_free_small(ptr[i], S0);
    }
    cprintf("批量%u字节分配/释放(%d 个)通过\n", (unsigned)S0, BULK);

    // 4) 多页分配（走页级 PMM）
    struct Page *pg = slub_alloc_pages(NPG);
    assert(pg != NULL);
    cprintf("分配%u页 OK，Page*=%p\n", (unsigned)NPG, pg);
    slub_free_pages(pg, NPG);
    cprintf("释放%u页 OK\n", (unsigned)NPG);

    cprintf("测试结束\n");
}
const struct pmm_manager slub_pmm_manager = {
    .name = "slub_pmm_manager",
    .init = slub_init,
    .init_memmap = slub_init_memmap,
    .alloc_pages = slub_alloc,
    .free_pages = slub_free,
    .nr_free_pages = slub_nr_free_pages,
    .check = slub_check,
};
