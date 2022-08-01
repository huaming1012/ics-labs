
/*
 * mm-naive.c - The fastest, least memory-efficient malloc package.
 * 
 * In this naive approach, a block is allocated by simply incrementing
 * the brk pointer.  A block is pure payload. There are no headers or
 * footers.  Blocks are never coalesced or reused. Realloc is
 * implemented directly using mm_malloc and mm_free.
 *
 * NOTE TO STUDENTS: Replace this header comment with your own header
 * comment that gives a high level description of your solution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

/*********************************************************
 * NOTE TO STUDENTS: Before you do anything else, please
 * provide your team information in the following struct.
 ********************************************************/
team_t team = {
    /* Team name */
    "ateam",
    /* First member's full name */
    "Li Xinyu",
    /* First member's email address */
    "bovik@cs.cmu.edu",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""
};

/* single word (4) or double word (8) alignment */
#define ALIGNMENT 8

/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~0x7)  //根据对齐规则舍入
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))  //舍入后的大小

#define WSIZE 4  //单字
#define DSIZE 8  //双字
#define CHUNKSIZE (1<<12)  //每次扩展堆的大小时申请的内存

#define MAX(x,y) ((x)>(y)? (x):(y)) 

//块头部和脚部存储的值，包括块大小和是否分配
#define PACK(size, alloc) ((size) | (alloc))  

#define GET(p) (*(unsigned int *)(p))  //得到p地址处的内容
#define PUT(p, val) (*(unsigned int *)(p) = (val))  //向p地址存入值val

#define GET_SIZE(p) (GET(p) & ~0x7)  //从头部或脚部得到块的大小
#define GET_ALLOC(p) (GET(p) & 0x1)  //从头部或脚部得到块是否已分配

//bp指向块的payload的开始
#define HDRP(bp) ((char*)(bp) - WSIZE) //得到块头部的地址
#define FTRP(bp) ((char*)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)//得到块脚部的地址
//得到下一个块的payload起始地址
#define NEXT_BLKP(bp) ((char*)(bp) + GET_SIZE(((char*)(bp) - WSIZE))) 
//得到上一个块的地址
#define PREV_BLKP(bp) ((char*)(bp) - GET_SIZE(((char*)(bp) - DSIZE))) 

static char* heap_list; //堆头
static char* pre_list;

static void* extend_heap(size_t words);  //在最末尾扩展堆
static void* coalesce(void* bp);  //合并空闲块
static void* find_fit(size_t asize);  //first-fit算法
static void place(void* bp, size_t asize); //分割空闲块

void* extend_heap(size_t words){
    char* bp;
    size_t realsize;
    
    realsize = (words % 2) ? (words+1) * WSIZE : words * WSIZE;

    if((long)(bp = mem_sbrk(realsize)) == -1){
        return NULL; //申请分配失败，返回NULL
    }

    //申请分配成功，设置新分配块的头部脚部和新的结尾块
    PUT(HDRP(bp), PACK(realsize, 0));  //新分配的空闲块头部
    PUT(FTRP(bp), PACK(realsize, 0));  //新分配的空闲块脚部
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); //新结尾块，大小0，已分配

    return coalesce(bp);
}

void* coalesce(void* bp){
    size_t front_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));  //前一个块的分配状况
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t cur_size = GET_SIZE(HDRP(bp));

    if(front_alloc && next_alloc){  //如果前后块都不为空闲块，直接返回
        pre_list = bp;
        return bp;
    }else if(front_alloc && !next_alloc){//如果前一块不空，后一块为空,与后面的块合并
        cur_size += GET_SIZE(HDRP(NEXT_BLKP(bp)));  //当前块大小增加
        PUT(HDRP(bp), PACK(cur_size, 0));  //先修改头部
        PUT(FTRP(bp), PACK(cur_size, 0));  //再修改尾部，因为依靠头部的信息
    }else if(!front_alloc && next_alloc){
        cur_size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(cur_size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(cur_size, 0));
        bp = PREV_BLKP(bp); 
    }else{
        cur_size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(cur_size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(cur_size, 0));
        bp = PREV_BLKP(bp);
    }
    pre_list = bp;
    return bp;
}

void* find_fit(size_t asize){
    char* bp = pre_list;
    size_t isalloc;
    size_t size;
    while (GET_SIZE(HDRP(NEXT_BLKP(bp))) > 0) {
        bp = NEXT_BLKP(bp);
        isalloc = GET_ALLOC(HDRP(bp));
        if (isalloc) continue;
        size = GET_SIZE(HDRP(bp));
        if (size < asize) continue;
        return bp;
    }
    bp = heap_list;
    while (bp != pre_list) {
        bp = NEXT_BLKP(bp);
        isalloc = GET_ALLOC(HDRP(bp));
        if (isalloc) continue;
        size = GET_SIZE(HDRP(bp));
        if (size < asize) continue;
        return bp;
    } 
    return NULL;
}


void place(void* bp, size_t asize){
    size_t csize = GET_SIZE(HDRP(bp));
    
    /* 判断是否能够分离空闲块 */
    if((csize - asize) >= 2*DSIZE) {
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
    
        PUT(HDRP(NEXT_BLKP(bp)), PACK(csize - asize, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(csize - asize, 0));
    }
    /* 设置为填充 */
    else{
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
        
    }
    pre_list = bp;
}

/* 
 * mm_init - initialize the malloc package.
 */
int mm_init(void)
{
    //尝试申请四字节空间
    if((heap_list = mem_sbrk(4*WSIZE)) == (void*)-1)
        return -1;
    PUT(heap_list, 0); //申请成功后对齐

    PUT(heap_list + (1 * WSIZE), PACK(DSIZE, 1));  //序言块头部，大小为8的已分配块
    PUT(heap_list + (2 * WSIZE), PACK(DSIZE, 1));  //序言块脚部
    PUT(heap_list + (3 * WSIZE), PACK(0, 1));  //结尾块，大小为0的已分配块

    heap_list += DSIZE;
    pre_list = heap_list;
    if(extend_heap((1 << 6)/WSIZE) == NULL)
        return -1;

    return 0;
}

/* 
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
void *mm_malloc(size_t size)
{
    /*int newsize = ALIGN(size + SIZE_T_SIZE);
    void *p = mem_sbrk(newsize);
    if (p == (void *)-1)
	return NULL;
    else {
        *(size_t *)p = size;
        return (void *)((char *)p + SIZE_T_SIZE);
    }*/
    size_t asize;
    size_t extendsize;
    char *bp;
    if(size == 0)
        return NULL;
    if(size <= DSIZE)
        asize = 2*DSIZE;
    else
        asize = DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE);
    /* 寻找合适的空闲块 */
    if((bp = find_fit(asize)) != NULL){
        place(bp, asize);
        return bp;
    }
    /* 找不到则扩展堆 */
    extendsize = MAX(asize, CHUNKSIZE);
    if((bp = extend_heap(extendsize/WSIZE)) == NULL)
        return NULL;
    place(bp, asize);
    return bp;
}

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *ptr)
{
    //if(ptr == NULL)
    //    return;
    size_t size = GET_SIZE(HDRP(ptr));

    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));
    coalesce(ptr);
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *mm_realloc(void *ptr, size_t size)
{
    if(size == 0){
        mm_free(ptr);
        return NULL;
    }
    if(ptr == NULL){
        return mm_malloc(size);
    }

    void *oldptr = ptr;
    void *newptr;
    void *nextptr;
    size_t copySize;
    size_t asize;
    size_t extendsize;

    if (size <= DSIZE)
   		asize = 2*DSIZE;
   	else 
   		asize = DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE);

    copySize = GET_SIZE(HDRP(ptr));
    
    if(asize == copySize){
        return ptr;
    }else if(asize < copySize){
        place(ptr, asize);
        return ptr;
    }else{
        nextptr = NEXT_BLKP(ptr);
        size_t newsize = GET_SIZE(HDRP(nextptr)) + copySize;
        if(!GET_ALLOC(HDRP(nextptr)) && newsize >= asize){
            PUT(HDRP(ptr), PACK(newsize, 0));
            place(ptr, asize);
            return ptr;
        }else{
            newptr = find_fit(asize);
            if(newptr == NULL){
                extendsize = MAX(asize, CHUNKSIZE);
                if((newptr = extend_heap(extendsize / WSIZE)) == NULL){
                    return NULL;
                }
            }
            place(newptr, asize);
            memcpy(newptr, oldptr, copySize - 2*WSIZE);
            mm_free(oldptr);
            return newptr;
        }
    }
}

