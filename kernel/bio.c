// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 2
#define HASH(id) (id%NBUCKET) /* 哈希运算得到映射值 */

struct hashbuf
{
  struct spinlock lock;/* 锁 */
  struct buf head;     /* 头结点 */
};
struct 
{
  struct buf buf[NBUF];
  struct hashbuf buckets[NBUCKET];/* 散列桶 */
} bcache;

void
binit(void)
{
  struct buf *b;
  char lockname[16];

  for(int i=0;i<NBUCKET;i++)
  {
    /* 1.初始化所有锁 */
    snprintf(lockname,sizeof(lockname),"bcache_%d",i);
    initlock(&bcache.buckets[i].lock,lockname);
    /* 2.初始化散列桶的头结点 */
    bcache.buckets[i].head.prev=&bcache.buckets[i].head;
    bcache.buckets[i].head.next=&bcache.buckets[i].head;
  }
  /* 3.将所有的缓冲区挂载到buckets[0]上 */
  // Create linked list of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.buckets[0].head.next;
    b->prev = &bcache.buckets[0].head;
    initsleeplock(&b->lock, "buffer");
    bcache.buckets[0].head.next->prev = b;
    bcache.buckets[0].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int bid=HASH(blockno);
  acquire(&bcache.buckets[bid].lock);

  // Is the block already cached?
  /* 这里先看一下这个block是否已经被缓存过 如果是直接返回就行 */
  for(b = bcache.buckets[bid].head.next; b != &bcache.buckets[bid].head; b = b->next)
  {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;

      /* 记录使用时间戳 */
      acquire(&tickslock);
      b->timestamp=ticks;
      release(&tickslock);

      release(&bcache.buckets[bid].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  b=0;
  struct buf* tmp;
  // Recycle the least recently used (LRU) unused buffer.
  /* 从当前散列桶开始寻找 */
  for(int i=bid,cycle=0;cycle!=NBUCKET;i=(i+1)%NBUCKET)
  {
    cycle++;
    if(i!=bid)
    {
      if(!holding(&bcache.buckets[i].lock))
        acquire(&bcache.buckets[i].lock);
      else
        continue;
    }
    for(tmp=bcache.buckets[i].head.next;tmp != &bcache.buckets[i].head; tmp = tmp->next)
    {
      // 使用时间戳进行LRU算法，而不是根据结点在链表中的位置
      if(tmp->refcnt == 0 && (b == 0 || tmp->timestamp < b->timestamp))
        b = tmp;
    }
    if(b)
    {
      // 如果是从其他散列桶窃取的，则将其以头插法插入到当前桶
      if(i!=bid)
      {
        /* 先从其他桶删除节点 */
        b->next->prev=b->prev;
        b->prev->next=b->next;
        release(&bcache.buckets[i].lock);
        /* 插入到当前桶 */
        b->next=bcache.buckets[bid].head.next;
        b->prev=&bcache.buckets[bid].head;
        bcache.buckets[bid].head.next->prev=b;
        bcache.buckets[bid].head.next=b;
      }
      b->blockno=blockno;
      b->dev=dev;
      b->valid=0;
      b->refcnt=1;

      /* 更新时间戳 */
      acquire(&tickslock);
      b->timestamp=ticks;
      release(&tickslock);

      release(&bcache.buckets[bid].lock);
      acquiresleep(&b->lock);
      return b;
    }
    else
    {
      /* 在当前桶中没找到可用的缓存 */
      if(i!=bid)
        release(&bcache.buckets[i].lock);
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bid=HASH(b->blockno);

  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;
  /* 更新时间戳 */
  // 由于LRU改为使用时间戳判定，不再需要头插法
  acquire(&tickslock);
  b->timestamp=ticks;
  release(&tickslock);
  
  release(&bcache.buckets[bid].lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.buckets[HASH(b->blockno)].lock);
  b->refcnt++;
  release(&bcache.buckets[HASH(b->blockno)].lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.buckets[HASH(b->blockno)].lock);
  b->refcnt--;
  release(&bcache.buckets[HASH(b->blockno)].lock);
}


