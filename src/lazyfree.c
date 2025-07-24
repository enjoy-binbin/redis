#include "server.h"
#include "bio.h"
#include "atomicvar.h"
#include "cluster.h"

static size_t lazyfree_objects = 0;
pthread_mutex_t lazyfree_objects_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Return the number of currently pending objects to free. */
size_t lazyfreeGetPendingObjectsCount(void) {
    size_t aux;
    atomicGet(lazyfree_objects,aux);
    return aux;
}

/* Return the amount of work needed in order to free an object.
 * The return value is not always the actual number of allocations the
 * object is compoesd of, but a number proportional to it.
 *
 * For strings the function always returns 1.
 *
 * For aggregated objects represented by hash tables or other data structures
 * the function just returns the number of elements the object is composed of.
 *
 * Objects composed of single allocations are always reported as having a
 * single item even if they are actaully logical composed of multiple
 * elements.
 *
 * For lists the funciton returns the number of elements in the quicklist
 * representing the list. */
size_t lazyfreeGetFreeEffort(robj *obj) {
    if (obj->type == OBJ_LIST) {
        quicklist *ql = obj->ptr;
        return ql->len;
    } else if (obj->type == OBJ_SET && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else if (obj->type == OBJ_ZSET && obj->encoding == OBJ_ENCODING_SKIPLIST){
        zset *zs = obj->ptr;
        return zs->zsl->length;
    } else if (obj->type == OBJ_HASH && obj->encoding == OBJ_ENCODING_HT) {
        dict *ht = obj->ptr;
        return dictSize(ht);
    } else {
        return 1; /* Everything else is a single allocation. */
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB.
 * If there are enough allocations to free the value object may be put into
 * a lazy free list instead of being freed synchronously. The lazy free list
 * will be reclaimed in a different bio.c thread. */
#define LAZYFREE_THRESHOLD 64
int dbAsyncDelete(redisDb *db, robj *key) {
    int slot = getKeySlot(key->ptr);

    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    if (dictSize(db->expires[slot]) > 0) {
        if (dictDelete(db->expires[slot],key->ptr) == DICT_OK) {
            cumulativeKeyCountAdd(db, slot, -1, DB_EXPIRES);
            db->sub_dict[DB_EXPIRES].key_count--;
        }
    }

    /* If the value is composed of a few allocations, to free in a lazy way
     * is actually just slower... So under a certain limit we just free
     * the object synchronously. */
    dictEntry *de = dictUnlink(db->dict[slot],key->ptr);
    if (de) {
        robj *val = dictGetVal(de);
        size_t free_effort = lazyfreeGetFreeEffort(val);
        cumulativeKeyCountAdd(db, slot, -1, DB_MAIN);
        db->sub_dict[DB_MAIN].key_count--;

        /* If releasing the object is too much work, do it in the background
         * by adding the object to the lazy free list.
         * Note that if the object is shared, to reclaim it now it is not
         * possible. This rarely happens, however sometimes the implementation
         * of parts of the Redis core may call incrRefCount() to protect
         * objects, and then call dbDelete(). In this case we'll fall
         * through and reach the dictFreeUnlinkedEntry() call, that will be
         * equivalent to just calling decrRefCount(). */
        if (free_effort > LAZYFREE_THRESHOLD && val->refcount == 1) {
            atomicIncr(lazyfree_objects,1);
            bioCreateBackgroundJob(BIO_LAZY_FREE,val,NULL,NULL);
            dictSetVal(db->dict[slot],de,NULL);
        }
    }

    /* Release the key-val pair, or just the key if we set the val
     * field to NULL in order to lazy free it later. */
    if (de) {
        dictFreeUnlinkedEntry(db->dict[slot],de);
        return 1;
    } else {
        return 0;
    }
}

/* Empty a Redis DB asynchronously. What the function does actually is to
 * create a new empty set of hash tables and scheduling the old ones for
 * lazy freeing. */
void emptyDbAsync(redisDb *db) {
    dict **oldDict = db->dict;
    dict **oldExpires = db->expires;
    atomicIncr(lazyfree_objects,dbSize(db, DB_MAIN));
    db->dict = dictCreateMultiple(&dbDictType, db->dict_count);
    db->expires = dictCreateMultiple(&dbExpiresDictType, db->dict_count);
    int *count = zmalloc(sizeof(int));
    *count = db->dict_count;
    bioCreateBackgroundJob(BIO_LAZY_FREE,count,oldDict,oldExpires);
}

/* Release objects from the lazyfree thread. It's just decrRefCount()
 * updating the count of objects to release. */
void lazyfreeFreeObjectFromBioThread(robj *o) {
    decrRefCount(o);
    atomicDecr(lazyfree_objects,1);
}

/* Release a database from the lazyfree thread. The 'db' pointer is the
 * database which was substitutied with a fresh one in the main thread
 * when the database was logically deleted. 'sl' is a skiplist used by
 * Redis Cluster in order to take the hash slots -> keys mapping. This
 * may be NULL if Redis Cluster is disabled. */
void lazyfreeFreeDatabaseFromBioThread(void *arg1, void *arg2, void *arg3) {
    int *dictCount = (int *) arg1;
    dict **ht1 = (dict **) arg2;
    dict **ht2 = (dict **) arg3;
    for (int i=0; i<*dictCount; i++) {
        size_t numkeys = dictSize(ht1[i]);
        dictRelease(ht1[i]);
        dictRelease(ht2[i]);
        atomicDecr(lazyfree_objects,numkeys);
    }
    zfree(ht1);
    zfree(ht2);
    zfree(dictCount);
}
