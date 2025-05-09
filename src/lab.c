#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "lab.h"

#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

/* btok: this function accepts a number of bytes and calculates the associated kval
 * bytes: the number of bytes
 */
size_t btok(size_t bytes)
{
    if (bytes == 0) {
        fprintf(stderr, "byte size 0 passed to btok\n");
    }
    size_t targetBytes = bytes - UINT64_C(1);
    size_t kval = UINT64_C(0);
    while (targetBytes != UINT64_C(0)) {
        kval++;
        targetBytes = targetBytes >> UINT64_C(1);
    }
    return kval;
}

/* buddy_calc: This function accepts a pool and buddy and returns a pointer to the buddy block
 * pool: the buddy_pool
 * buddy: the buddy to calculate the buddy of
 */
struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    if (pool == NULL || buddy == NULL) { 
        fprintf(stderr,"null pointer passed to buddy_calc");
        return NULL;
    }
    uintptr_t buddyBuddy = (uintptr_t)buddy - (uintptr_t)pool->base;
    buddyBuddy = buddyBuddy ^ UINT64_C(1) << buddy->kval;
    return (struct avail *)((uintptr_t)pool->base + buddyBuddy);
}

/* buddy_malloc: this function allocates the smallest block to fit the byte size requested
 * pool: the pool to malloc in
 * size: the requested size in bytes
 */
void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    if (pool == NULL || size == 0) {
        fprintf(stderr, "null pointer passed to buddy_malloc\n");
        return NULL;
    }
    //get the kval for the requested size with enough room for the tag and kval fields
    size_t kval = btok(size + sizeof(struct avail));
    if (kval < SMALLEST_K) {
        return NULL;
    }
    //R1 Find a block
    size_t j = UINT64_MAX;
    for (size_t i = kval; i <= pool->kval_m; i++) {
        if (pool->avail[i].next != &pool->avail[i]) { //block found
            j = i;
            break;
        }
    }
    //There was not enough memory to satisfy the request thus we need to set error and return NULL
    if (j == UINT64_MAX) {
        errno = ENOMEM;
        return NULL;
    }
    //R2 Remove from list;
    struct avail* L = pool->avail[j].next;
    struct avail* availNext = L->next;
    pool->avail[j].next = availNext;
    availNext->prev = &pool->avail[j];
    L->tag = BLOCK_RESERVED;
    //R3 Split required?
    while (j != kval) {  
        //R4 Split
        j--;
        struct avail* P = (struct avail*)((uintptr_t)L + (UINT64_C(1)<<j));
        P->tag = 1;
        P->kval = j;
        P->next = P->prev = &pool->avail[j];
        pool->avail[j].next = pool->avail[j].prev = P;
    }
    L->kval = j;
    return L + 1;
    
}

/*
    CODE REVIEW: updated the while loop to be more clear.
*/

/* buddy_free: this function accepts a pointer and frees it from the pool
 * pool: the buddy_pool to free the pointer from
 * ptr: the pointer to free
 */
void buddy_free(struct buddy_pool *pool, void *ptr)
{
    if (pool == NULL || ptr == NULL) {
        fprintf(stderr,"null pointer passed to buddy_free\n");
        return;
    }
    //get back to the start of the block from the ptr passed to free
    struct avail* L = (struct avail*) ptr - 1;
    struct avail* P = buddy_calc(pool, L);
    //S1 Is buddy available?
    while(L->kval < pool->kval_m && P->tag == BLOCK_AVAIL) {
        //S2 Combine with buddy.
        P->prev->next = P->next;
        P->next->prev = P->prev;
        if (P < L) { L = P; }
        L->kval++;
        P = buddy_calc(pool, L);
    }
    //S3 Put on list
    L->tag = BLOCK_AVAIL;
    P = pool->avail[L->kval].next;
    L->next = P;
    P->prev = L;
    L->prev = &pool->avail[L->kval];
    pool->avail[L->kval].next = L;
}

/* buddy_realloc: this function accepts a pointer and new size and reallocates the pointer to the new size
 * pool: the buddy_pool to realloc within
 * ptr: the pointer to realloc
 * size: the size to realloc to
 */
void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size)
{
    //Required for Grad Students
    //Optional for Undergrad Students
    if (pool == NULL) {
        fprintf(stderr, "null pointer pool passed to buddy_realloc\n");
        return NULL;
    }
    if (ptr == NULL) {
        return buddy_malloc(pool, size);
    }
    if (size == UINT64_C(0)) {
       buddy_free(pool, ptr);
       return NULL;
    }
    //otherwise we need to realloc
    struct avail* currentBlock = (struct avail*) (ptr - sizeof(struct avail));
    //if the size they need is within the same block size, just return
    size_t neededKVal = btok(size + sizeof(struct avail));
    size_t currentKVal = currentBlock->kval;
    //if need the same size block
    if (currentBlock->kval == neededKVal) { return ptr; }
    //if need a bigger block
    else if (currentBlock->kval < neededKVal) {
       //try to combine with buddy
        while(true) {
            struct avail* P = buddy_calc(pool, currentBlock);
            if (currentBlock->kval == neededKVal || currentBlock->kval == pool->kval_m || P->tag == BLOCK_RESERVED 
                || (P->tag == BLOCK_AVAIL && P->kval != currentBlock->kval)) { break; }
            //S2 Combine with buddy.
            P->prev->next = P->next;
            P->next->prev = P->prev;
            if (P < currentBlock) { 
                memcpy(P + 1,currentBlock + 1, UINT64_C(1) << currentKVal);
                currentBlock = P; 
            }
            currentBlock->kval++;
        }
        if (currentBlock->kval == neededKVal) { return (void*)currentBlock + sizeof(struct avail); }
        //otherwise just get a bigger block
        else {
            struct avail* newMalloc = buddy_malloc(pool, size);
            memcpy(newMalloc,currentBlock, UINT64_C(1) << currentKVal);
            buddy_free(pool, currentBlock);
        }
        //set enomem if can't
        errno = ENOMEM;
        return NULL;
    }
    //if need a smaller block
    else {
        //need a smaller block
        while (currentKVal != neededKVal) {  
            currentKVal--;
            struct avail* P = (struct avail*)(((uintptr_t)currentBlock) + (1<<currentKVal));
            P->tag = 1;
            P->kval = currentKVal;
            P->next = P->prev = &pool->avail[currentKVal];
            pool->avail[currentKVal].next = pool->avail[currentKVal].prev = P;
        }
        currentBlock->kval = currentKVal;
        return (void*) (currentBlock + 1);
    }
}

/* buddy_init: this function initializes the buddy_pool
 * Written by Professor Panter
 * pool: the pool to init
 * size: the size of memory to manage
 */
void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;

    //make sure pool struct is cleared out
    memset(pool,0,sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    //Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                               /*addr to map to*/
        pool->numbytes,                     /*length*/
        PROT_READ | PROT_WRITE,             /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS,        /*flags*/
        -1,                                 /*fd -1 when using MAP_ANONYMOUS*/
        0                                   /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    //Set all blocks to empty. We are using circular lists so the first elements just point
    //to an available block. Thus the tag, and kval feild are unused burning a small bit of
    //memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    //Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

/* buddy_destroy: this destroys a buddy_pool and associated data
 * Written by Shane Panter
 * pool: the buddy_pool to destroy
 */
void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    //Zero out the array so it can be reused it needed
    memset(pool,0,sizeof(struct buddy_pool));
}

/**
 * This function can be useful to visualize the bits in a block. This can
 * help when figuring out the buddy_calc function!
 */
// static void printb(unsigned long int b)
// {
//      size_t bits = sizeof(b) * 8;
//      unsigned long int curr = UINT64_C(1) << (bits - 1);
//      for (size_t i = 0; i < bits; i++)
//      {
//           if (b & curr)
//           {
//                printf("1");
//           }
//           else
//           {
//                printf("0");
//           }
//           curr >>= 1L;
//      }
// }
