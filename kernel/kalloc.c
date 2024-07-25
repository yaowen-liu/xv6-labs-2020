// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

struct ref_stru
{
  struct spinlock lock;
  int cnt[PHYSTOP/PGSIZE];/* 引用计数 */
}ref;

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  /* 初始化ref的自旋锁 */
  initlock(&ref.lock, "ref");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  {
    ref.cnt[(uint64)p/PGSIZE]=1;
    kfree(p);
  }
 
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  /* 只有当引用计数为0时才回收空间 否则只是将引用计数减1*/
  acquire(&ref.lock);
  if(--ref.cnt[(uint64)pa/PGSIZE]==0)
  {
    release(&ref.lock);
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
  else
  {
    release(&ref.lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
  {
    kmem.freelist = r->next;
    /* 申请内存时将引用计数初始化为1 */
    acquire(&ref.lock);
    ref.cnt[(uint64)r/PGSIZE]=1;
    release(&ref.lock);
  }
    
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

/**
 * @description: 获取指定内存的引用计数
 * @param  pa 指定的内存物理地址
 * @return  引用计数
 */
int krefcnt(void* pa)
{
  /* 思考：这里是否需要加锁 */
  return ref.cnt[(uint64)pa/PGSIZE];
}

/**
 * @description: 增加指定内存的引用计数
 * @param  pa 指定的内存物理地址
 * @return  0成功 -1失败
 */
int kaddrefcnt(void* pa)
{
  if(((uint64)pa%PGSIZE)!=0||(char*)pa<end||(uint64)pa>=PHYSTOP)
    return -1;
  acquire(&ref.lock);
  ++ref.cnt[(uint64)pa/PGSIZE];
  release(&ref.lock);
  return 0;
}

/**
 * @description: cowpage 判断一个页面是否为COW页面
 * @param pagetable 指定查询的页表
 * @param va 虚拟地址
 * @return 0 是 -1 不是
 */
int cowpage(pagetable_t pagetable, uint64 va) 
{
  if(va >= MAXVA)
    return -1;
  pte_t* pte = walk(pagetable, va, 0);
  if(pte == 0)
    return -1;
  if((*pte & PTE_V) == 0)
    return -1;
  return (*pte & PTE_F ? 0 : -1);
}

/**
 * @description: cowalloc copy on write分配器
 * @param  pagetable 指定页表
 * @param  va 指定虚拟地址 必须页面对齐
 * @return  返回分配后va对应的物理地址 返回0则失败
 */
void* cowalloc(pagetable_t pagetable,uint64 va)
{
  /* 判断是否已经页面对齐 */
  if(va%PGSIZE!=0)
    return 0;
  /* 获取相应的物理地址 */
  uint64 pa=walkaddr(pagetable,va);
  if(pa==0)
    return 0;
  /* 获取对应的pte */
  pte_t* pte=walk(pagetable,va,0);
  
  if(krefcnt((void*)pa)==1)
  {
    /* 只剩一个进程对该物理地址存在引用 则直接修改pte即可 */
    *pte|=PTE_W;
    *pte&=~PTE_F;
    return (void*)pa;
  }
  else
  {
    /* 多个进程对该物理地址存在引用 需要分配新的页面并拷贝旧页面的内容 */
    char* mem=kalloc();
    if(mem==0)
      return 0;
    /* 复制旧页面内容到新页面 */
    memmove(mem,(char*)pa,PGSIZE);
    /* 清除PTE_V 否则在mappages中会判定为remap */
    *pte&=~PTE_V;
    /* 为新页面添加映射 */
    if(mappages(pagetable,va,PGSIZE,(uint64)mem,(PTE_FLAGS(*pte)|PTE_W)&~PTE_F)!=0)
    {
      kfree(mem);
      *pte|=PTE_V;
      return 0;
    }
    /* 将原来的物理内存引用计数-1 */
    kfree((char*)PGROUNDDOWN(pa));
    return mem;
  }
} 