/**
 * @file cache.h
 * @brief Header file for cache implementation
 *
 * Description: Max cache size, object size defines, function prototypes and
 * cache block structures defined here
 *
 *
 * @author Abhishek Basrithaya <abasrith@andrew.cmu.edu>
 */
#include "csapp.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
/*
 * Max cache and object sizes
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

typedef struct {
    char *cache_obj; /* point to web object with max size of MAX_OBJECT_SIZE */
    size_t cache_obj_size; /* Cache object size */
    char *cache_uri_key;   /* point to URI key */

    int readReferenceCnt; /*count of references to the block */
    void *nextBlock;      /* Points to next cache block */
    void *previousBlock;  /* Points to previous cache block */
} cache_block;

typedef struct {
    cache_block *cacheBlockHead;
    cache_block *cacheBlockTail;
    size_t cache_size;
    pthread_mutex_t rwMutex; /*protects accesses to cache*/
} Cache;

/* Function prototyping */
void cache_init();
cache_block *cache_find(char *url);
void cache_eviction(size_t reqBufSize);
void cache_uri(char *uri, char *buf, size_t bufLen);
void cachePrint();
void lockMutex();
void unLockMutex();
