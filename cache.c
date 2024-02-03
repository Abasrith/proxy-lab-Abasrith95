/**
 * @file cache.c
 * @brief Basic LRU cache definition to handle client web requests
 *
 * Description: An LRU cache with max cache size of MAX_CACHE_SIZE, where each
 * object cached has a max size of MAX_OBJECT_SIZE each, a doubly linked list is
 * utlised to add to cache line and evit from cache line. new server response
 * objects are cached at the tail and evicction is carried out from the head of
 * the implicit list. Single global mutex lock is utilised for thread
 * synchronisation.
 *
 *
 * @author Abhishek Basrithaya <abasrith@andrew.cmu.edu>
 */
#include "cache.h"
#include "csapp.h"
#include <string.h>

/* Global cache structure */
Cache cache;

/**
 * @brief Initialises an LRU cache
 *
 *
 * @return      void
 */
void cache_init() {
    cache.cache_size = 0;
    cache.cacheBlockHead = NULL;
    cache.cacheBlockTail = NULL;
    if ((pthread_mutex_init(&cache.rwMutex, NULL)) != 0) {
        fprintf(stderr, "Error: Initizing mutex");
    }
}

/**
 * @brief Acquires lock to global mutex for thread synchronisation
 *
 *
 * @return      void
 */
void lockMutex() {
    if ((pthread_mutex_lock(&cache.rwMutex)) != 0) {
        fprintf(stderr, "Error: Locking mutex");
    }
}
/**
 * @brief Releases lock to global mutex for thread synchronisation
 *
 *
 * @return      void
 */
void unLockMutex() {
    if ((pthread_mutex_unlock(&cache.rwMutex)) != 0) {
        fprintf(stderr, "Error: Unlocking mutex");
    }
}
/**
 * @brief returns a cache block if server object was present in the cache or
 * else returns NULL
 *
 *
 * @param[in]   *char           URL key to search in the LRU cache
 *
 * @return      cache_block*    Pointer to cache block with cached server
 * object, NULL for cache miss
 */
cache_block *cache_find(char *url) {

    lockMutex();
    cache_block *cacheLinePtr = cache.cacheBlockHead;
    cache_block *tempCachePtr = NULL;
    while (cacheLinePtr != NULL) {
        if (strcmp(url, cacheLinePtr->cache_uri_key) == 0) {
            cacheLinePtr->readReferenceCnt++;
            /* Implicit list update */
            /* cache hit at head */
            if ((cacheLinePtr == cache.cacheBlockHead) &&
                (cacheLinePtr->nextBlock != NULL)) {
                cache.cacheBlockHead = cacheLinePtr->nextBlock;
                tempCachePtr = cacheLinePtr->nextBlock;
                tempCachePtr->previousBlock = NULL;

                tempCachePtr = cache.cacheBlockTail;
                cache.cacheBlockTail = cacheLinePtr;
                tempCachePtr->nextBlock = cacheLinePtr;
                cacheLinePtr->nextBlock = NULL;
                cacheLinePtr->previousBlock = tempCachePtr;

            }
            /* cache hit at tail */
            else if ((cacheLinePtr == cache.cacheBlockTail) &&
                     (cacheLinePtr->previousBlock != NULL)) {
                /* cache hit at tail, do nothing */
            }
            /* cache hit in the middle */
            else if ((cacheLinePtr->previousBlock != NULL) &&
                     (cacheLinePtr->nextBlock != NULL)) {
                tempCachePtr = cacheLinePtr->nextBlock;
                tempCachePtr->previousBlock = cacheLinePtr->previousBlock;

                tempCachePtr = cacheLinePtr->previousBlock;
                tempCachePtr->nextBlock = cacheLinePtr->nextBlock;

                tempCachePtr = cache.cacheBlockTail;
                cache.cacheBlockTail = cacheLinePtr;
                tempCachePtr->nextBlock = cacheLinePtr;
                cacheLinePtr->nextBlock = NULL;
                cacheLinePtr->previousBlock = tempCachePtr;
            } else {
                /* cache hit only block, do nothing */
            }
            break;
        }
        cacheLinePtr = cacheLinePtr->nextBlock;
    }

    if (cacheLinePtr == NULL) {
        unLockMutex();
        return NULL; /*can not find url in the cache*/
    } else {
        unLockMutex();
        return cacheLinePtr;
    }
}
/**
 * @brief Performs cache eviction till required cache size is freed. Cache
 * eviction starts at the head
 *
 *
 * @param[in]   reqBufSize           Size to be evicted
 *
 * @return      void
 */
void cache_eviction(size_t reqBufSize) {
    cache_block *tempCacheLinePtr = NULL;
    size_t sizeFreed = 0;
    cache_block *cacheLinePtr = cache.cacheBlockHead;
    while ((sizeFreed < reqBufSize) && (cacheLinePtr != NULL)) {

        /* Busy wait till reference count becomes 0 */
        while (cacheLinePtr->readReferenceCnt != 0) {
            unLockMutex();
            lockMutex();
        }
        Free(cacheLinePtr->cache_obj);
        Free(cacheLinePtr->cache_uri_key);
        /* Implicit list update */
        /* not the only block present */
        if (cacheLinePtr->nextBlock != NULL) {
            cache.cacheBlockHead = cacheLinePtr->nextBlock;
            tempCacheLinePtr = cacheLinePtr->nextBlock;
            tempCacheLinePtr->previousBlock = NULL;
        }
        /* only block present */
        else {
            cache.cacheBlockHead = NULL;
            cache.cacheBlockTail = NULL;
        }
        cache.cache_size -= cacheLinePtr->cache_obj_size;
        sizeFreed += cacheLinePtr->cache_obj_size;
        Free(cacheLinePtr);

        cacheLinePtr = cache.cacheBlockHead;
    }
    if (sizeFreed < reqBufSize)
        fprintf(stderr, "Error: Size freed=%ld, size required=%ld\n", sizeFreed,
                reqBufSize);
}
/**
 * @brief Performs cache addition starting at the head
 *
 *
 * @param[in]   *uri          URL to be cached
 * @param[in]   *buf          Server response to be cached
 * @param[in]   buffSize      Server response size to be cached
 *
 * @return      void
 */
void cache_uri(char *uri, char *buf, size_t buffSize) {

    lockMutex();
    cache_block *cacheLinePtr = NULL;
    cache_block *tempCacheLinePtr = NULL;
    size_t updatedtotalCacheSize = cache.cache_size + buffSize;
    if (updatedtotalCacheSize > MAX_CACHE_SIZE) {
        cache_eviction((cache.cache_size + buffSize) - MAX_CACHE_SIZE);
    }
    /* Allocate memory for cache block and object */
    cacheLinePtr = Malloc(sizeof(cache_block));
    cacheLinePtr->cache_obj = Malloc(buffSize);

    /* Update implicit list */
    /* If head & tail are NULL - Initialise */
    if ((cache.cacheBlockHead == NULL) && (cache.cacheBlockTail == NULL)) {
        cache.cacheBlockTail = cacheLinePtr;
        cache.cacheBlockHead = cache.cacheBlockTail;
        cacheLinePtr->previousBlock = NULL;
        cacheLinePtr->nextBlock = NULL;
    } else {
        tempCacheLinePtr = cache.cacheBlockTail;
        cache.cacheBlockTail = cacheLinePtr;
        cacheLinePtr->previousBlock = tempCacheLinePtr;
        tempCacheLinePtr->nextBlock = cacheLinePtr;
        cacheLinePtr->nextBlock = NULL;
    }
    /* Copy object and URL */
    memcpy(cacheLinePtr->cache_obj, buf, buffSize);
    cacheLinePtr->cache_obj_size = buffSize;
    cacheLinePtr->cache_uri_key = uri;
    cacheLinePtr->readReferenceCnt = 0;
    cache.cache_size += buffSize;
    unLockMutex();
}
/**
 * @brief Prints the LRU cache structure
 *
 *
 * @return      void
 */
void cachePrint() {
    cache_block *cacheLinePtr = cache.cacheBlockHead;
    int i = 0;
    while (cacheLinePtr != NULL) {
        sio_printf("cacheLine-URL[%d] size:%ld = %s\n", i,
                   cacheLinePtr->cache_obj_size, cacheLinePtr->cache_uri_key);
        /*sio_printf("CacheLine-Object[%d] = %s\n", i,
         * cacheLinePtr->cache_obj);*/
        cacheLinePtr = cacheLinePtr->nextBlock;
        i++;
    }
    /*sio_printf("cache.cache_size=%ld\n", cache.cache_size);*/
}
