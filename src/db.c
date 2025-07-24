/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "cluster.h"
#include "atomicvar.h"

#include <signal.h>
#include <ctype.h>

/* Structure for DB iterator that allows iterating across multiple slot specific dictionaries in cluster mode. */
struct dbIterator {
    redisDb *db;
    int slot;
    int next_slot;
    dictIterator di;
    dbKeyType keyType;
};

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

dict *dbGetDictFromIterator(dbIterator *dbit) {
    if (dbit->keyType == DB_MAIN)
        return dbit->db->dict[dbit->slot];
    else if (dbit->keyType == DB_EXPIRES)
        return dbit->db->expires[dbit->slot];
    else
        serverAssert(0);
}

/* Returns next dictionary from the iterator, or NULL if iteration is complete. */
dict *dbIteratorNextDict(dbIterator *dbit) {
    if (dbit->next_slot == -1) return NULL;
    dbit->slot = dbit->next_slot;
    dbit->next_slot = dbGetNextNonEmptySlot(dbit->db, dbit->slot, dbit->keyType);
    return dbGetDictFromIterator(dbit);
}

int dbIteratorGetCurrentSlot(dbIterator *dbit) {
    serverAssert(dbit->slot >= 0 && dbit->slot < CLUSTER_SLOTS);
    return dbit->slot;
}

/* Returns next entry from the multi slot db. */
dictEntry *dbIteratorNext(dbIterator *dbit) {
    dictEntry *de = dbit->di.d ? dictNext(&dbit->di) : NULL;
    if (!de) { /* No current dict or reached the end of the dictionary. */
        dict *d = dbIteratorNextDict(dbit);
        if (!d) return NULL;
        dictInitSafeIterator(&dbit->di, d);
        de = dictNext(&dbit->di);
    }
    return de;
}

/* Returns DB iterator that can be used to iterate through sub-dictionaries.
 * Primary database contains only one dictionary when node runs without cluster mode,
 * or 16k dictionaries (one per slot) when node runs with cluster mode enabled. */
dbIterator *dbIteratorInit(redisDb *db, dbKeyType keyType) {
    dbIterator *dbit = zmalloc(sizeof(*dbit));
    dbit->db = db;
    dbit->slot = -1;
    dbit->keyType = keyType;
    dbit->next_slot = findSlotByKeyIndex(dbit->db, 1, dbit->keyType); /* Finds first non-empty slot. */
    dictInitSafeIterator(&dbit->di, NULL);
    return dbit;
}

dbIterator *dbIteratorInitFromSlot(redisDb *db, dbKeyType keyType, int slot) {
    dbIterator *dbit = zmalloc(sizeof(*dbit));
    dbit->db = db;
    dbit->slot = slot;
    dbit->keyType = keyType;
    dbit->next_slot = dbGetNextNonEmptySlot(dbit->db, dbit->slot, dbit->keyType);
    dictInitSafeIterator(&dbit->di, NULL);
    return dbit;
}

/* Returns next non-empty slot strictly after given one, or -1 if provided slot is the last one. */
int dbGetNextNonEmptySlot(redisDb *db, int slot, dbKeyType keyType) {
    unsigned long long next_key = cumulativeKeyCountRead(db, slot, keyType) + 1;
    return next_key <= dbSize(db, keyType) ? findSlotByKeyIndex(db, next_key, keyType) : -1;
}

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. */
void updateLFU(robj *val) {
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* Low level key lookup API, not actually called directly from commands
 * implementations that should instead rely on lookupKeyRead(),
 * lookupKeyWrite() and lookupKeyReadWithFlags(). */
robj *lookupKey(redisDb *db, robj *key, int flags) {
    dictEntry *de = dbFind(db, key->ptr, DB_MAIN);
    if (de) {
        robj *val = dictGetVal(de);

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        if (server.rdb_child_pid == -1 &&
            server.aof_child_pid == -1 &&
            !(flags & LOOKUP_NOTOUCH))
        {
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
                val->lru = LRU_CLOCK();
            }
        }
        return val;
    } else {
        return NULL;
    }
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * As a side effect of calling this function:
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): no special flags are passed.
 *  LOOKUP_NOTOUCH: don't alter the last access time of the key.
 *
 * Note: this function also returns NULL is the key is logically expired
 * but still existing, in case this is a slave, since this API is called only
 * for read operations. Even if the key expiry is master-driven, we can
 * correctly report a key is expired on slaves even if the master is lagging
 * expiring our key via DELs in the replication link. */
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    robj *val;

    if (expireIfNeeded(db,key) == 1) {
        /* Key expired. If we are in the context of a master, expireIfNeeded()
         * returns 0 only when the key does not exist at all, so it's safe
         * to return NULL ASAP. */
        if (server.masterhost == NULL) return NULL;

        /* However if we are in the context of a slave, expireIfNeeded() will
         * not really try to expire the key, it only returns information
         * about the "logical" status of the key: key expiring is up to the
         * master in order to have a consistent view of master's data set.
         *
         * However, if the command caller is not the master, and as additional
         * safety measure, the command invoked is a read-only command, we can
         * safely return NULL here, and provide a more consistent behavior
         * to clients accessign expired values in a read-only fashion, that
         * will say the key as non exisitng.
         *
         * Notably this covers GETs when slaves are used to scale reads. */
        if (server.current_client &&
            server.current_client != server.master &&
            server.current_client->cmd &&
            server.current_client->cmd->flags & CMD_READONLY)
        {
            return NULL;
        }
    }
    val = lookupKey(db,key,flags);
    if (val == NULL)
        server.stat_keyspace_misses++;
    else
        server.stat_keyspace_hits++;
    return val;
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. */
robj *lookupKeyWrite(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key,LOOKUP_NONE);
}

robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
void dbAdd(redisDb *db, robj *key, robj *val) {
    sds copy = sdsdup(key->ptr);
    int slot = getKeySlot(key->ptr);
    dict *d = db->dict[slot];
    dictEntry *de = dictAddRaw(d, copy, NULL);
    serverAssertWithInfo(NULL, key, de != NULL);
    dictSetVal(d, de, val);
    db->sub_dict[DB_MAIN].key_count++;
    cumulativeKeyCountAdd(db, slot, 1, DB_MAIN);
    if (val->type == OBJ_LIST) signalListAsReady(db, key);
 }

/* Returns key's hash slot when cluster mode is enabled, or 0 when disabled.
 * The only difference between this function and getKeySlot, is that it's not using cached key slot from the current_client
 * and always calculates CRC hash.
 * This is useful when slot needs to be calculated for a key that user didn't request for, such as in case of eviction. */
int calculateKeySlot(sds key) {
    return server.cluster_enabled ? keyHashSlot(key, (int) sdslen(key)) : 0;
}

/* Return slot-specific dictionary for key based on key's hash slot when cluster mode is enabled, else 0.*/
int getKeySlot(sds key) {
    /* This is performance optimization that uses pre-set slot id from the current command,
     * in order to avoid calculation of the key hash.
     * This optimization is only used when current_client flag `CLIENT_EXECUTING_COMMAND` is set.
     * It only gets set during the execution of command under `call` method. Other flows requesting
     * the key slot would fallback to calculateKeySlot.
     */
    if (server.current_client && server.current_client->slot >= 0 && server.current_client->flags & CLIENT_EXECUTING_COMMAND) {
        return server.current_client->slot;
    }
    return calculateKeySlot(key);
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    int slot = getKeySlot(key->ptr);
    dictEntry *de = dictFind(db->dict[slot],key->ptr);

    serverAssertWithInfo(NULL,key,de != NULL);
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        robj *old = dictGetVal(de);
        int saved_lru = old->lru;
        dictReplace(db->dict[slot], key->ptr, val);
        val->lru = saved_lru;
        /* LFU should be not only copied but also updated
         * when a key is overwritten. */
        updateLFU(val);
    } else {
        dictReplace(db->dict[slot], key->ptr, val);
    }
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent).
 *
 * All the new keys in the database should be craeted via this interface. */
void setKey(redisDb *db, robj *key, robj *val) {
    if (lookupKeyWrite(db,key) == NULL) {
        dbAdd(db,key,val);
    } else {
        dbOverwrite(db,key,val);
    }
    incrRefCount(val);
    removeExpire(db,key);
    signalModifiedKey(db,key);
}

int dbExists(redisDb *db, robj *key) {
    int slot = getKeySlot(key->ptr);
    return dictFind(db->dict[slot],key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
    int maxtries = 100;
    int allvolatile = dbSize(db, DB_MAIN) == dbSize(db, DB_EXPIRES);

    while(1) {
        sds key;
        robj *keyobj;
        int randomSlot = getFairRandomSlot(db, DB_MAIN);
        de = dictGetFairRandomKey(db->dict[randomSlot]);
        if (de == NULL) return NULL;

        key = dictGetKey(de);
        keyobj = createStringObject(key,sdslen(key));
        if (dbFind(db, key, DB_EXPIRES)) {
            if (allvolatile && server.masterhost && --maxtries == 0) {
                /* If the DB is composed only of keys with an expire set,
                 * it could happen that all the keys are already logically
                 * expired in the slave, so the function cannot stop because
                 * expireIfNeeded() is false, nor it can stop because
                 * dictGetRandomKey() returns NULL (there are keys to return).
                 * To prevent the infinite loop we do some tries, but if there
                 * are the conditions for an infinite loop, eventually we
                 * return a key name that may be already expired. */
                return keyobj;
            }
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        return keyobj;
    }
}

/* Updates binary index tree (also known as Fenwick tree), increasing key count for a given slot.
 * You can read more about this data structure here https://en.wikipedia.org/wiki/Fenwick_tree
 * Time complexity is O(log(CLUSTER_SLOTS)). */
void cumulativeKeyCountAdd(redisDb *db, int slot, long delta, dbKeyType keyType) {
    if (!server.cluster_enabled) return;
    int idx = slot + 1; /* Unlike slots, BIT is 1-based, so we need to add 1. */
    while (idx <= CLUSTER_SLOTS) {
        if (delta < 0) {
            serverAssert(db->sub_dict[keyType].slot_size_index[idx] >= (unsigned long long)labs(delta));
        }
        db->sub_dict[keyType].slot_size_index[idx] += delta;
        idx += (idx & -idx);
    }
}

/* Returns total (cumulative) number of keys up until given slot (inclusive).
 * Time complexity is O(log(CLUSTER_SLOTS)). */
unsigned long long cumulativeKeyCountRead(redisDb *db, int slot, dbKeyType keyType) {
    if (!server.cluster_enabled) {
        serverAssert(slot == 0);
        return dbSize(db, keyType);
    }
    int idx = slot + 1;
    unsigned long long sum = 0;
    while (idx > 0) {
        sum += db->sub_dict[keyType].slot_size_index[idx];
        idx -= (idx & -idx);
    }
    return sum;
}

/* Returns fair random slot, probability of each slot being returned is proportional to the number of elements that slot dictionary holds.
 * Implementation uses binary search on top of binary index tree.
 * This function guarantees that it returns a slot whose dict is non-empty, unless the entire db is empty.
 * Time complexity of this function is O(log^2(CLUSTER_SLOTS)). */
int getFairRandomSlot(redisDb *db, dbKeyType keyType) {
    unsigned long target = dbSize(db, keyType) ? (random() % dbSize(db, keyType)) + 1 : 0;
    int slot = findSlotByKeyIndex(db, target, keyType);
    return slot;
}

static inline unsigned long dictSizebySlot(redisDb *db, int slot, dbKeyType keyType) {
    if (keyType == DB_MAIN)
        return dictSize(db->dict[slot]);
    else if (keyType == DB_EXPIRES)
        return dictSize(db->expires[slot]);
    else
        serverAssert(0);
}

/* Finds a slot containing target element in a key space ordered by slot id.
 * Consider this example. Slots are represented by brackets and keys by dots:
 *  #0   #1   #2     #3    #4
 * [..][....][...][.......][.]
 *                    ^
 *                 target
 *
 * In this case slot #3 contains key that we are trying to find.
 *
 * This function is 1 based and the range of the target is [1..dbSize], dbSize inclusive.
 * Time complexity of this function is O(log^2(CLUSTER_SLOTS))
 * */
int findSlotByKeyIndex(redisDb *db, unsigned long target, dbKeyType keyType) {
    if (!server.cluster_enabled || dbSize(db, keyType) == 0) return 0;
    serverAssert(target <= dbSize(db, keyType));
    int lo = 0, hi = CLUSTER_SLOTS - 1;
    /* We use binary search to find a slot, we are allowed to do this, because we have a quick way to find a total number of keys
     * up until certain slot, using binary index tree. */
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        unsigned long keys_up_to_mid = cumulativeKeyCountRead(db, mid, keyType); /* Total number of keys up until a given slot (inclusive). */
        unsigned long keys_in_mid = dictSizebySlot(db, mid, keyType);
        if (target > keys_up_to_mid) { /* Target is to the right from mid. */
            lo = mid + 1;
        } else if (target <= keys_up_to_mid - keys_in_mid)  { /* Target is to the left from mid. */
            hi = mid - 1;
        } else { /* Located target. */
            return mid;
        }
    }
    serverPanic("Unable to find a slot that contains target key.");
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
int dbSyncDelete(redisDb *db, robj *key) {
    int slot = getKeySlot(key->ptr);

    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    if (dictSize(db->expires[slot]) > 0) {
        if (dictDelete(db->expires[slot],key->ptr) == DICT_OK) {
            cumulativeKeyCountAdd(db, slot, -1, DB_EXPIRES);
            db->sub_dict[DB_EXPIRES].key_count--;
        }
    }
    dictEntry *de = dictUnlink(db->dict[slot],key->ptr);
    if (de) {
        dictFreeUnlinkedEntry(db->dict[slot],de);
        cumulativeKeyCountAdd(db, slot, -1, DB_MAIN);
        db->sub_dict[DB_MAIN].key_count--;
        return 1;
    } else {
        return 0;
    }
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. */
int dbDelete(redisDb *db, robj *key) {
    return server.lazyfree_lazy_server_del ? dbAsyncDelete(db,key) :
                                             dbSyncDelete(db,key);
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbOverwrite(db,key,o);
    }
    return o;
}

/* Remove all keys from all the databases in a Redis server.
 * If callback is given the function is called from time to time to
 * signal that work is in progress.
 *
 * The dbnum can be -1 if all teh DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP.
 *
 * On success the fuction returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
long long emptyDb(int dbnum, int flags, void(callback)(void*)) {
    int j, async = (flags & EMPTYDB_ASYNC);
    long long removed = 0;

    if (dbnum < -1 || dbnum >= server.dbnum) {
        errno = EINVAL;
        return -1;
    }

    redisDb *dbarray = server.db;
    for (j = 0; j < server.dbnum; j++) {
        if (dbnum != -1 && dbnum != j) continue;
        removed += dbSize(&dbarray[j], DB_MAIN);
        if (async) {
            emptyDbAsync(&server.db[j]);
        } else {
            for (int k = 0; k < dbarray[j].dict_count; k++) {
                dictEmpty(dbarray[j].dict[k],callback);
                dictEmpty(dbarray[j].expires[k],callback);
            }
        }
        for (dbKeyType subdict = DB_MAIN; subdict <= DB_EXPIRES; subdict++) {
            dbarray[j].sub_dict[subdict].key_count = 0;
            dbarray[j].sub_dict[subdict].resize_cursor = 0;
            if (server.cluster_enabled) {
                unsigned long long *slot_size_index = dbarray[j].sub_dict[subdict].slot_size_index;
                memset(slot_size_index, 0, sizeof(unsigned long long) * (CLUSTER_SLOTS + 1));
            }
        }
    }
    if (dbnum == -1) flushSlaveKeysWithExpireList();
    return removed;
}

int selectDb(client *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
    c->db = &server.db[id];
    return C_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

void signalModifiedKey(redisDb *db, robj *key) {
    touchWatchedKey(db,key);
}

void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

/* Return the set of flags to use for the emptyDb() call for FLUSHALL
 * and FLUSHDB commands.
 *
 * Currently the command just attempts to parse the "ASYNC" option. It
 * also checks if the command arity is wrong.
 *
 * On success C_OK is returned and the flags are stored in *flags, otherwise
 * C_ERR is returned and the function sends an error to the client. */
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. */
    if (c->argc > 1) {
        if (c->argc > 2 || strcasecmp(c->argv[1]->ptr,"async")) {
            addReply(c,shared.syntaxerr);
            return C_ERR;
        }
        *flags = EMPTYDB_ASYNC;
    } else {
        *flags = EMPTYDB_NO_FLAGS;
    }
    return C_OK;
}

/* FLUSHDB [ASYNC]
 *
 * Flushes the currently SELECTed Redis DB. */
void flushdbCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    signalFlushedDb(c->db->id);
    server.dirty += emptyDb(c->db->id,flags,NULL);
    addReply(c,shared.ok);
}

/* FLUSHALL [ASYNC]
 *
 * Flushes the whole server data set. */
void flushallCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    signalFlushedDb(-1);
    server.dirty += emptyDb(-1,flags,NULL);
    addReply(c,shared.ok);
    if (server.rdb_child_pid != -1) {
        kill(server.rdb_child_pid,SIGUSR1);
        rdbRemoveTempFile(server.rdb_child_pid);
    }
    if (server.saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        int saved_dirty = server.dirty;
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        rdbSave(server.rdb_filename,rsiptr);
        server.dirty = saved_dirty;
    }
    server.dirty++;
}

/* This command implements DEL and LAZYDEL. */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    for (j = 1; j < c->argc; j++) {
        expireIfNeeded(c->db,c->argv[j]);
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {
            signalModifiedKey(c->db,c->argv[j]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            numdel++;
        }
    }
    addReplyLongLong(c,numdel);
}

void delCommand(client *c) {
    delGenericCommand(c,0);
}

void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. */
void existsCommand(client *c) {
    long long count = 0;
    int j;

    for (j = 1; j < c->argc; j++) {
        if (lookupKeyRead(c->db,c->argv[j])) count++;
    }
    addReplyLongLong(c,count);
}

void selectCommand(client *c) {
    long id;

    if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != C_OK)
        return;

    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
    } else {
        addReply(c,shared.ok);
    }
}

void randomkeyCommand(client *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

void keysCommand(client *c) {
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addDeferredMultiBulkLength(c);
    dbIterator *dbit = dbIteratorInit(c->db, DB_MAIN);

    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while ((de = dbIteratorNext(dbit)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj;

        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    zfree(dbit);
    setDeferredMultiBulkLength(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];
    robj *o = pd[1];
    robj *key, *val = NULL;

    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        sds keysds = dictGetKey(de);
        key = createStringObject(keysds,sdslen(keysds));
    } else if (o->type == OBJ_HASH) {
        sds sdskey = dictGetKey(de);
        sds sdsval = dictGetVal(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObject(sdsval,sdslen(sdsval));
    } else if (o->type == OBJ_ZSET) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. */
int parseScanCursorOrReply(client *c, robj *o, unsigned long long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash or Set object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash. */
void scanGenericCommand(client *c, robj *o, unsigned long long cursor) {
    int i, j;
    list *keys = listCreate();
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* Step 1: Parse options. */
    while (i < c->argc) {
        j = c->argc - i;
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                goto cleanup;
            }

            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;
            patlen = sdslen(pat);

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. */
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a ziplist, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */

    /* Handle the case of a hash table. */
    ht = NULL;
    if (o == NULL) {
        ht = NULL;
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. */
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
        count *= 2; /* We return key / value for this type. */
    }

    /* For main dictionary scan or data structure using hashtable. */
    if (!o || ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
        privdata[0] = keys;
        privdata[1] = o;
        do {
            /* In cluster mode there is a separate dictionary for each slot.
             * If cursor is empty, we should try exploring next non-empty slot. */
            if (o == NULL) {
                cursor = dbScan(c->db, DB_MAIN, cursor, scanCallback, NULL, privdata);
            } else {
                cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
            }
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (unsigned long)count);
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;

        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;

        while(p) {
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys,
                (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
                                 createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter elements. */
    node = listFirst(keys);
    while (node) {
        robj *kobj = listNodeValue(node);
        nextnode = listNextNode(node);
        int filter = 0;

        /* Filter element if it does not match the pattern. */
        if (!filter && use_pattern) {
            if (sdsEncodedObject(kobj)) {
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter element if it is an expired key. */
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associted value if needed. */
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);
            if (filter) {
                kobj = listNodeValue(node);
                decrRefCount(kobj);
                listDelNode(keys, node);
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client. */
    addReplyMultiBulkLen(c, 2);
    addReplyBulkLongLong(c,cursor);

    addReplyMultiBulkLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);
    listRelease(keys);
}

void addSlotIdToCursor(int slot, unsigned long long *cursor) {
    if (!server.cluster_enabled) return;
    /* Slot id can be -1 when iteration is over and there are no more slots to visit. */
    if (slot < 0) return;
    *cursor = (*cursor << CLUSTER_SLOT_MASK_BITS) | slot;
}

int getAndClearSlotIdFromCursor(unsigned long long *cursor) {
    if (!server.cluster_enabled) return 0;
    int slot = (int) (*cursor & CLUSTER_SLOT_MASK);
    *cursor = *cursor >> CLUSTER_SLOT_MASK_BITS;
    return slot;
}

/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(client *c) {
    unsigned long long cursor;
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    scanGenericCommand(c,NULL,cursor);
}

void dbsizeCommand(client *c) {
    redisDb *db = c->db;
    unsigned long long int size = dbSize(db, DB_MAIN);
    addReplyLongLong(c, size);
}

unsigned long long int dbSize(redisDb *db, dbKeyType keyType) {
    return db->sub_dict[keyType].key_count;
}

/* This method proivdes the cumulative sum of all the dictionary buckets
 * across dictionaries in a database. */
unsigned long dbBuckets(redisDb *db, dbKeyType keyType) {
    unsigned long buckets = 0;
    dict *d;
    dbIterator *dbit = dbIteratorInit(db, keyType);
    while ((d = dbIteratorNextDict(dbit))) {
        buckets += dictSlots(d);
    }
    zfree(dbit);
    return buckets;
}

size_t dbMemUsage(redisDb *db, dbKeyType keyType) {
    size_t mem = 0;
    unsigned long long keys_count = dbSize(db, keyType);
    mem += keys_count * dictEntryMemUsage() +
           dbBuckets(db, keyType) * sizeof(dictEntry*) +
           db->dict_count * sizeof(dict);
    if (keyType == DB_MAIN) {
        mem+=keys_count * sizeof(robj);
    }
    return mem;
}

dictEntry *dbFind(redisDb *db, void *key, dbKeyType keyType){
    int slot = getKeySlot(key);
    if (keyType == DB_MAIN) {
        return dictFind(db->dict[slot], key);
    }
    else if (keyType == DB_EXPIRES)
        return dictFind(db->expires[slot], key);
    else
        serverAssert(0);
}

/*
 * This method is used to iterate over the elements of the entire database specifically across slots.
 * It's a three pronged approach.
 *
 * 1. It uses the provided cursor `v` to retrieve the slot from it.
 * 2. If the dictionary is in a valid state checked through the provided callback `dictScanValidFunction`,
 *    it performs a dictScan over the appropriate `keyType` dictionary of `db`.
 * 3. If the slot is entirely scanned i.e. the cursor has reached 0, the next non empty slot is discovered.
 *    The slot information is embedded into the cursor and returned.
 */
unsigned long long dbScan(redisDb *db, dbKeyType keyType, unsigned long long v, dictScanFunction *fn, int (dictScanValidFunction)(dict *d), void *privdata) {
    dict *d;
    unsigned long long cursor = 0;
    /* During main dictionary traversal in cluster mode, 48 lower bits in the cursor are used for positioning in the HT.
     * Following 14 bits are used for the slot number, ranging from 0 to 2^14-1.
     * Slot is always 0 at the start of iteration and can be incremented only in cluster mode. */
    int slot = getAndClearSlotIdFromCursor(&v);
    if (keyType == DB_MAIN)
        d = db->dict[slot];
    else if (keyType == DB_EXPIRES)
        d = db->expires[slot];
    else
        serverAssert(0);

    int is_dict_valid = (dictScanValidFunction == NULL || dictScanValidFunction(d) == C_OK);
    if (is_dict_valid) {
        cursor = dictScan(d, v, fn, NULL, privdata);
    } else {
        serverLog(LL_DEBUG, "Slot [%d] not valid for scanning, skipping.", slot);
    }
    /* scanning done for the current dictionary or if the scanning wasn't possible, move to the next slot. */
    if (cursor == 0 || !is_dict_valid) {
        slot = dbGetNextNonEmptySlot(db, slot, keyType);
    }
    if (slot == -1) {
        return 0;
    }
    addSlotIdToCursor(slot, &cursor);
    return cursor;
}

void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

void typeCommand(client *c) {
    robj *o;
    char *type;

    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case OBJ_STRING: type = "string"; break;
        case OBJ_LIST: type = "list"; break;
        case OBJ_SET: type = "set"; break;
        case OBJ_ZSET: type = "zset"; break;
        case OBJ_HASH: type = "hash"; break;
        case OBJ_MODULE: {
            moduleValue *mv = o->ptr;
            type = mv->type->name;
        }; break;
        default: type = "unknown"; break;
        }
    }
    addReplyStatus(c,type);
}

void shutdownCommand(client *c) {
    int flags = 0;

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == 2) {
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
            flags |= SHUTDOWN_SAVE;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;
    if (prepareForShutdown(flags) == C_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed,
     * if the key exists, however we still return an error on unexisting key. */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) samekey = 1;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }

    incrRefCount(o);
    expire = getExpire(c->db,c->argv[1]);
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        dbDelete(c->db,c->argv[2]);
    }
    dbAdd(c->db,c->argv[2],o);
    if (expire != -1) setExpire(c,c->db,c->argv[2],expire);
    dbDelete(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    long long dbid, expire;

    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    src = c->db;
    srcid = c->db->id;

    if (getLongLongFromObject(c->argv[2],&dbid) == C_ERR ||
        dbid < INT_MIN || dbid > INT_MAX ||
        selectDb(c,dbid) == C_ERR)
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
    dst = c->db;
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    expire = getExpire(c->db,c->argv[1]);

    /* Return zero if the key already exists in the target DB */
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    dbAdd(dst,c->argv[1],o);
    if (expire != -1) setExpire(c,dst,c->argv[1],expire);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    dbDelete(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other list blocking commands
 * and signal the keys are ready if they are lists. See the comment where
 * the function is used for more info. */
void scanDatabaseForReadyLists(redisDb *db) {
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
        if (value && value->type == OBJ_LIST)
            signalListAsReady(db, key);
    }
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see
 * the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need
 * to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise
 * C_OK is returned. */
int dbSwapDatabases(int id1, int id2) {
    if (id1 < 0 || id1 >= server.dbnum ||
        id2 < 0 || id2 >= server.dbnum) return C_ERR;
    if (id1 == id2) return C_OK;
    redisDb aux = server.db[id1];
    redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];

    /* Swap hash tables. Note that we don't swap blocking_keys,
     * ready_keys and watched_keys, since we want clients to
     * remain in the same DB they were. */
    db1->dict = db2->dict;
    db1->expires = db2->expires;
    db1->avg_ttl = db2->avg_ttl;

    db2->dict = aux.dict;
    db2->expires = aux.expires;
    db2->avg_ttl = aux.avg_ttl;

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready
     * if needed. */
    scanDatabaseForReadyLists(db1);
    scanDatabaseForReadyLists(db2);
    return C_OK;
}

/* SWAPDB db1 db2 */
void swapdbCommand(client *c) {
    long id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. */
    if (server.cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. */
    if (getLongFromObjectOrReply(c, c->argv[1], &id1,
        "invalid first DB index") != C_OK)
        return;

    if (getLongFromObjectOrReply(c, c->argv[2], &id2,
        "invalid second DB index") != C_OK)
        return;

    /* Swap... */
    if (dbSwapDatabases(id1,id2) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    } else {
        server.dirty++;
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    // serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    if (dictDelete(db->expires[(getKeySlot(key->ptr))],key->ptr) == DICT_OK) {
        db->sub_dict[DB_EXPIRES].key_count--;
        cumulativeKeyCountAdd(db, getKeySlot(key->ptr), -1, DB_EXPIRES);
        return 1;
    } else {
        return 0;
    }
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
void setExpire(client *c, redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de, *existing;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dbFind(db, key->ptr, DB_MAIN);
    serverAssertWithInfo(NULL,key,kde != NULL);
    int slot = getKeySlot(key->ptr);
    de = dictAddRaw(db->expires[slot], dictGetKey(kde), &existing);
    if (existing) {
        dictSetSignedIntegerVal(existing, when);
    } else {
        dictSetSignedIntegerVal(de, when);
        db->sub_dict[DB_EXPIRES].key_count++;
        cumulativeKeyCountAdd(db, slot, 1, DB_EXPIRES);
    }

    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires[getKeySlot(key->ptr)]) == 0 ||
        (de = dbFind(db,key->ptr, DB_EXPIRES)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    // serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
void propagateExpire(redisDb *db, robj *key, int lazy) {
    robj *argv[2];

    argv[0] = lazy ? shared.unlink : shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    if (server.aof_state != AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
    replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because slave instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the slave side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
int expireIfNeeded(redisDb *db, robj *key) {
    mstime_t when = getExpire(db,key);
    mstime_t now;

    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
    now = server.lua_caller ? server.lua_time_start : mstime();

    /* If we are running in the context of a slave, return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    if (server.masterhost != NULL) return now > when;

    /* Return when this key has not expired */
    if (now <= when) return 0;

    /* Delete the key */
    server.stat_expiredkeys++;
    propagateExpire(db,key,server.lazyfree_lazy_expire);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,
        "expired",key,db->id);
    return server.lazyfree_lazy_expire ? dbAsyncDelete(db,key) :
                                         dbSyncDelete(db,key);
}

/*
 * This functions increases size of the main/expires db to match desired number.
 * In cluster mode resizes all individual dictionaries for slots that this node owns.
 *
 * Based on the parameter `try_expand`, appropriate dict expand API is invoked.
 * if try_expand is set to 1, `dictTryExpand` is used else `dictExpand`.
 * The return code is either `DICT_OK`/`DICT_ERR` for both the API(s).
 * `DICT_OK` response is for successful expansion. However ,`DICT_ERR` response signifies failure in allocation in
 * `dictTryExpand` call and in case of `dictExpand` call it signifies no expansion was performed.
 */
int dbExpand(const redisDb *db, uint64_t db_size, dbKeyType keyType, int try_expand) {
    dict *d;
    if (server.cluster_enabled) {
        for (int i = 0; i < CLUSTER_SLOTS; i++) {
            if (clusterNodeGetSlotBit(server.cluster->myself, i)) {
                /* We don't know exact number of keys that would fall into each slot, but we can approximate it, assuming even distribution. */
                if (keyType == DB_MAIN) {
                    d = db->dict[i];
                } else {
                    d = db->expires[i];
                }
                int result = try_expand ? dictTryExpand(d, db_size) : dictExpand(d, db_size);
                if (try_expand && result == DICT_ERR) {
                    serverLog(LL_WARNING, "Dict expansion failed for type :%s slot: %d",
                              keyType == DB_MAIN ? "main" : "expires", i);
                    return C_ERR;
                } else if (result == DICT_ERR) {
                    serverLog(LL_DEBUG, "Dict expansion skipped for type :%s slot: %d",
                              keyType == DB_MAIN ? "main" : "expires", i);
                }
            }
        }
    } else {
        if (keyType == DB_MAIN) {
            d = db->dict[0];
        } else {
            d = db->expires[0];
        }
        int result = try_expand ? dictTryExpand(d, db_size) : dictExpand(d, db_size);
        if (try_expand && result == DICT_ERR) {
            serverLog(LL_WARNING, "Dict expansion failed for db type: %s",
                      keyType == DB_MAIN ? "main" : "expires");
            return C_ERR;
        }
    }
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). */
int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys) {
    int j, i = 0, last, *keys;
    UNUSED(argv);

    if (cmd->firstkey == 0) {
        *numkeys = 0;
        return NULL;
    }

    last = cmd->lastkey;
    if (last < 0) last = argc+last;
    keys = zmalloc(sizeof(int)*((last - cmd->firstkey)+1));
    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        if (j >= argc) {
            /* Modules commands, and standard commands with a not fixed number
             * of arugments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                zfree(keys);
                *numkeys = 0;
                return NULL;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i++] = j;
    }
    *numkeys = i;
    return keys;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is an heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,numkeys);
    } else if (!(cmd->flags & CMD_MODULE) && cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,numkeys);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

/* Free the result of getKeysFromCommand. */
void getKeysFreeResult(int *result) {
    zfree(result);
}

/* Helper function to extract keys from following commands:
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 * ZINTERSTORE <destkey> <num-keys> <key> <key> ... <key> <options> */
int *zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    /* Keys in z{union,inter}store come from two places:
     * argv[1] = storage key,
     * argv[3...n] = keys to intersect */
    keys = zmalloc(sizeof(int)*(num+1));

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    /* Finally add the argv[1] key position (the storage key target). */
    keys[num] = 1;
    *numkeys = num+1;  /* Total keys = {union,inter} keys + storage key */
    return keys;
}

/* Helper function to extract keys from the following commands:
 * EVAL <script> <num-keys> <key> <key> ... <key> [more stuff]
 * EVALSHA <script> <num-keys> <key> <key> ... <key> [more stuff] */
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    keys = zmalloc(sizeof(int)*num);
    *numkeys = num;

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    return keys;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. */
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, j, num, *keys, found_store = 0;
    UNUSED(cmd);

    num = 0;
    keys = zmalloc(sizeof(int)*2); /* Alloc 2 places for the worst case. */

    keys[num++] = 1; /* <sort-key> is always present. */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;
                keys[num] = i+1; /* <store-key> */
                break;
            }
        }
    }
    *numkeys = num + found_store;
    return keys;
}

int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, first, *keys;
    UNUSED(cmd);

    /* Assume the obvious form. */
    first = 3;
    num = 1;

    /* But check for the extended one with the KEYS option. */
    if (argc > 6) {
        for (i = 6; i < argc; i++) {
            if (!strcasecmp(argv[i]->ptr,"keys") &&
                sdslen(argv[3]->ptr) == 0)
            {
                first = i+1;
                num = argc-first;
                break;
            }
        }
    }

    keys = zmalloc(sizeof(int)*num);
    for (i = 0; i < num; i++) keys[i] = first+i;
    *numkeys = num;
    return keys;
}

/* Helper function to extract keys from following commands:
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                             [COUNT count] [STORE key] [STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ... */
int *georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command */
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = argv[i]->ptr;
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept 
         * the same as in georadiusCommand method.
         */
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * argv[1] = key,
     * argv[5...n] = stored key if present
     */
    keys = zmalloc(sizeof(int) * num);

    /* Add all key positions to keys[] */
    keys[0] = 1;
    if(num > 1) {
         keys[1] = stored_key;
    }
    *numkeys = num; 
    return keys;
}

void dbGetStats(char *buf, size_t bufsize, redisDb *db, int full, dbKeyType keyType) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;
    dictStats *mainHtStats = NULL;
    dictStats *rehashHtStats = NULL;
    dict *d;
    dbIterator *dbit = dbIteratorInit(db, keyType);
    while ((d = dbIteratorNextDict(dbit))) {
        dictStats *stats = dictGetStatsHt(d, 0, full);
        if (!mainHtStats) {
            mainHtStats = stats;
        } else {
            dictCombineStats(stats, mainHtStats);
            dictFreeStats(stats);
        }
        if (dictIsRehashing(d)) {
            stats = dictGetStatsHt(d, 1, full);
            if (!rehashHtStats) {
                rehashHtStats = stats;
            } else {
                dictCombineStats(stats, rehashHtStats);
                dictFreeStats(stats);
            }
        }
    }
    zfree(dbit);
    l = dictGetStatsMsg(buf, bufsize, mainHtStats, full);
    dictFreeStats(mainHtStats);
    buf += l;
    bufsize -= l;
    if (rehashHtStats && bufsize > 0) {
        dictGetStatsMsg(buf, bufsize, rehashHtStats, full);
        dictFreeStats(rehashHtStats);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize - 1] = '\0';
}
