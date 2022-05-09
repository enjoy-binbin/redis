/* quicklist.h - A generic doubly linked quicklist implementation
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
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

#include <stdint.h> // for UINTPTR_MAX

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

/* Node, quicklist, and Iterator are the only data structures used currently. */

/* quicklistNode is a 32 byte struct describing a ziplist for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * count: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, NONE=1, ZIPLIST=2.
 * recompress: 1 bit, bool, true if node is temporary decompressed for usage.
 * attempted_compress: 1 bit, boolean, used for verifying during testing.
 * extra: 10 bits, free for future use; pads out the remainder of 32 bits */
// quicklistNode 是一个 32 字节的结构，是一个在 quicklist 里的一个 ziplist
// 使用位域将它保持在 32 个字节里
typedef struct quicklistNode {
    // 指向前驱节点的指针，8 bytes
    struct quicklistNode *prev;
    // 指向后继节点的指针，8 bytes
    struct quicklistNode *next;
    // 数据指针，如果数据没有压缩，指向一个 ziplist 结构，否则指向一个 quicklistLZF 结构，8 bytes
    unsigned char *zl;
    // ziplist 的占用内存字节大小，即前面 ziplist 讲过的 zlbytes，4 bytes
    unsigned int sz;             /* ziplist size in bytes */

    // 下面这些字段使用位域，将总长度限制在了 32 位，也就是 4 个字节，将整个 quicklistNode 保持在 32 字节

    // 16 位，ziplist 里的节点数量，即前面 ziplist 讲过的 zllen
    unsigned int count : 16;     /* count of items in ziplist */
    // 2 位，ziplist 的编码方式，RAW 表示原生 ziplist，LZF 表示使用 LZF 对 ziplist 进行了压缩
    unsigned int encoding : 2;   /* RAW==1 or LZF==2 */
    // 2 位，数据容器，目前值都为 2，表示用的是 ziplist
    unsigned int container : 2;  /* NONE==1 or ZIPLIST==2 */
    // 1 位，如果为 true 表示临时使用，对这个节点进行了数据项的解压
    // 当类似使用 lindex 来查找某一项本来被压缩了的节点数据，此时就需要展示解压获取数据，之后再重新压缩
    unsigned int recompress : 1; /* was this node previous compressed? */
    // 1 位，目前是用于测试，数据能否被压缩
    unsigned int attempted_compress : 1; /* node can't compress; too small */
    // 10 位，用于扩展，而实际上一直到 7.0 也没有用上，32 - (16 + 2 + 2 + 1 + 1) = 10，填充到 32 位
    unsigned int extra : 10; /* more bits to steal for future usage */
} quicklistNode;

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->sz.
 * When quicklistNode->zl is compressed, node->zl points to a quicklistLZF */
// quicklistLZF 是一个 4+N 字节大小的结构体，在 sz 后面紧跟着压缩后的字符数组
// 压缩后的 ziplist 大小存储在 LZF->sz 中，未压缩的长度是存储在 quicklistNode->sz 里
// 如果 ziplist 有进行压缩，node->zl 指向 quicklistLZF，没压缩则指向 ziplist
typedef struct quicklistLZF {
    // 对 ziplist 进行 LZF 压缩后大内存占用大小，即后面 compressed 字节数组的长度，4 bytes
    unsigned int sz; /* LZF size in bytes*/
    // 存放压缩后的 ziplist 字节数组，是个柔性数组（不指定大小，需要放在成员最后）
    char compressed[];
} quicklistLZF;

/* Bookmarks are padded with realloc at the end of of the quicklist struct.
 * They should only be used for very big lists if thousands of nodes were the
 * excess memory usage is negligible, and there's a real need to iterate on them
 * in portions.
 * When not used, they don't add any memory overhead, but when used and then
 * deleted, some overhead remains (to avoid resonance).
 * The number of bookmarks used should be kept to minimum since it also adds
 * overhead on node deletion (searching for a bookmark to update). */
typedef struct quicklistBookmark {
    quicklistNode *node;
    char *name;
} quicklistBookmark;

#if UINTPTR_MAX == 0xffffffff
/* 32-bit */
#   define QL_FILL_BITS 14
#   define QL_COMP_BITS 14
#   define QL_BM_BITS 4
#elif UINTPTR_MAX == 0xffffffffffffffff
/* 64-bit */
#   define QL_FILL_BITS 16
#   define QL_COMP_BITS 16
#   define QL_BM_BITS 4 /* we can encode more, but we rather limit the user
                           since they cause performance degradation. */
#else
#   error unknown arch bits count
#endif

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: 0 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor.
 * 'bookmarks are an optional feature that is used by realloc this struct,
 *      so that they don't consume memory when not used. */
// quicklist 是一个 40 字节大小的结构体（64 位操作系统下），是一个双端链表
typedef struct quicklist {
    // 指向头节点的指针，8 bytes
    quicklistNode *head;
    // 指向尾节点的指针，8 bytes
    quicklistNode *tail;
    // 快速列表的元素数，所有 ziplists 中所有节点的总数，8 bytes
    unsigned long count;        /* total count of all entries in all ziplists */
    // 快速列表的节点数，8 bytes
    unsigned long len;          /* number of quicklistNodes */
    // 填充因子，存放 list-max-ziplist-size 参数值，16 位（64 操作系统下）
    int fill : QL_FILL_BITS;              /* fill factor for individual nodes */
    // 压缩配置，存放 list-compress-depth 参数值，16 位（64 操作系统下）
    unsigned int compress : QL_COMP_BITS; /* depth of end nodes not to compress;0=off */
    // bookmarks 数组的大小，4 位
    unsigned int bookmark_count: QL_BM_BITS;
    // 是一个可选的字段，用于重新分配 quicklist 的空间，不使用时不会消耗内存
    quicklistBookmark bookmarks[];
} quicklist;

typedef struct quicklistIter {
    const quicklist *quicklist;
    quicklistNode *current;
    unsigned char *zi;
    long offset; /* offset in current ziplist */
    int direction;
} quicklistIter;

// 用来描述 quicklist 中的每个 zlentry，其实就类似 ziplist 里面 zlentry 的使用，方便用来表示一个元素项
typedef struct quicklistEntry {
    // 指向该 zlentry 所在的 quicklist
    const quicklist *quicklist;
    // 指向该 zlentry 所在的 quicklistNode
    quicklistNode *node;
    // 指向该 zlentry 所在的 ziplist
    unsigned char *zi;
    // 如果 zlentry 数据类型为字符串时，value 和 sz 结合保存该值
    unsigned char *value;
    // 如果 zlentry 数据类型为整型, longval 保存了该值
    long long longval;
    // 该 zlentry 的 size
    unsigned int sz;
    // 该 zlentry 在 node->zl 中的偏移量，相当于标识是第几个数据项
    // eg: 第一个数据项 offset=0，最后一个数据项 offset=node->count
    int offset;
} quicklistEntry;

#define QUICKLIST_HEAD 0
#define QUICKLIST_TAIL -1

/* quicklist node encodings */
#define QUICKLIST_NODE_ENCODING_RAW 1
#define QUICKLIST_NODE_ENCODING_LZF 2

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0

/* quicklist container formats */
#define QUICKLIST_NODE_CONTAINER_NONE 1
#define QUICKLIST_NODE_CONTAINER_ZIPLIST 2

#define quicklistNodeIsCompressed(node)                                        \
    ((node)->encoding == QUICKLIST_NODE_ENCODING_LZF)

/* Prototypes */
quicklist *quicklistCreate(void);
quicklist *quicklistNew(int fill, int compress);
void quicklistSetCompressDepth(quicklist *quicklist, int depth);
void quicklistSetFill(quicklist *quicklist, int fill);
void quicklistSetOptions(quicklist *quicklist, int fill, int depth);
void quicklistRelease(quicklist *quicklist);
int quicklistPushHead(quicklist *quicklist, void *value, const size_t sz);
int quicklistPushTail(quicklist *quicklist, void *value, const size_t sz);
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,
                   int where);
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl);
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist,
                                            unsigned char *zl);
quicklist *quicklistCreateFromZiplist(int fill, int compress,
                                      unsigned char *zl);
void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *node,
                          void *value, const size_t sz);
void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *node,
                           void *value, const size_t sz);
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry);
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
                            int sz);
int quicklistDelRange(quicklist *quicklist, const long start, const long stop);
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction);
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist,
                                         int direction, const long long idx);
int quicklistNext(quicklistIter *iter, quicklistEntry *node);
void quicklistReleaseIterator(quicklistIter *iter);
quicklist *quicklistDup(quicklist *orig);
int quicklistIndex(const quicklist *quicklist, const long long index,
                   quicklistEntry *entry);
void quicklistRewind(quicklist *quicklist, quicklistIter *li);
void quicklistRewindTail(quicklist *quicklist, quicklistIter *li);
void quicklistRotate(quicklist *quicklist);
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz));
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 unsigned int *sz, long long *slong);
unsigned long quicklistCount(const quicklist *ql);
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len);
size_t quicklistGetLzf(const quicklistNode *node, void **data);

/* bookmarks */
int quicklistBookmarkCreate(quicklist **ql_ref, const char *name, quicklistNode *node);
int quicklistBookmarkDelete(quicklist *ql, const char *name);
quicklistNode *quicklistBookmarkFind(quicklist *ql, const char *name);
void quicklistBookmarksClear(quicklist *ql);

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[], int accurate);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */
