/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intset.h"
#include "zmalloc.h"
#include "endianconv.h"
#include "redisassert.h"

/* Note that these encodings are ordered, so:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

/* Return the required encoding for the provided value. */
static uint8_t _intsetValueEncoding(int64_t v) {
    if (v < INT32_MIN || v > INT32_MAX)
        return INTSET_ENC_INT64;
    else if (v < INT16_MIN || v > INT16_MAX)
        return INTSET_ENC_INT32;
    else
        return INTSET_ENC_INT16;
}

/* Return the value at pos, given an encoding. */
// 根据编码类型和 pos 下标进行偏移，从 intset->contents 里获取对应的整数值
static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {
    int64_t v64;
    int32_t v32;
    int16_t v16;

    if (enc == INTSET_ENC_INT64) {
        memcpy(&v64,((int64_t*)is->contents)+pos,sizeof(v64));
        memrev64ifbe(&v64);
        return v64;
    } else if (enc == INTSET_ENC_INT32) {
        memcpy(&v32,((int32_t*)is->contents)+pos,sizeof(v32));
        memrev32ifbe(&v32);
        return v32;
    } else {
        memcpy(&v16,((int16_t*)is->contents)+pos,sizeof(v16));
        memrev16ifbe(&v16);
        return v16;
    }
}

/* Return the value at pos, using the configured encoding. */
// 从 intset->contents 中获取指定 pos 下标位置的整数值
static int64_t _intsetGet(intset *is, int pos) {
    return _intsetGetEncoded(is,pos,intrev32ifbe(is->encoding));
}

/* Set the value at pos, using the configured encoding. */
// 在 intset->contents 指定 pos 下标设置整数值
static void _intsetSet(intset *is, int pos, int64_t value) {
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64) {
        // 按照编码方式对数组进行解释，然后直接在数组对应下标进行赋值
        ((int64_t*)is->contents)[pos] = value;
        memrev64ifbe(((int64_t*)is->contents)+pos);
    } else if (encoding == INTSET_ENC_INT32) {
        ((int32_t*)is->contents)[pos] = value;
        memrev32ifbe(((int32_t*)is->contents)+pos);
    } else {
        ((int16_t*)is->contents)[pos] = value;
        memrev16ifbe(((int16_t*)is->contents)+pos);
    }
}

/* Create an empty intset. */
intset *intsetNew(void) {
    intset *is = zmalloc(sizeof(intset));
    is->encoding = intrev32ifbe(INTSET_ENC_INT16);
    is->length = 0;
    return is;
}

/* Resize the intset */
// 根据 len 调整 intset 的大小
// 例如插入新元素扩容时 len = old_len + 1
// 例如删除元素缩容时 len = old_len - 1
// 例如 intset 升级，用新编码扩容 intset
static intset *intsetResize(intset *is, uint32_t len) {
    // 根据 len 和 encoding 计算出需要的 size 内存字节大小
    uint64_t size = (uint64_t)len*intrev32ifbe(is->encoding);
    // 一个断言防止整数溢出
    assert(size <= SIZE_MAX - sizeof(intset));
    // 根据新 size + intset header size 重新分配内存空间
    is = zrealloc(is,sizeof(intset)+size);
    return is;
}

/* Search for the position of "value". Return 1 when the value was found and
 * sets "pos" to the position of the value within the intset. Return 0 when
 * the value is not present in the intset and sets "pos" to the position
 * where "value" can be inserted. */
// 搜索指定值在 intset 中的位置，如果找到，则返回 1，并且设置 pos 的值为指定位置下标
// 如果没找到，则返回 0，并且设置 pos 为指定值可以插入的位置
// 搜索使用的是二分查找，所以时间复杂度为 O(log N)
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
    // 一些用于二分查找的辅助下标变量，基础知识
    int min = 0, max = intrev32ifbe(is->length)-1, mid = -1;
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    if (intrev32ifbe(is->length) == 0) {
        // 如果 intset 为空直接返回
        if (pos) *pos = 0;
        return 0;
    } else {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        // 此时根据 intset 的最小值和最大值，提前快速判断一下元素是否存在
        if (value > _intsetGet(is,max)) {
            // 如果 value 的值大于最大值即 intset[-1]，值不存在，插入的位置是 is->length
            if (pos) *pos = intrev32ifbe(is->length);
            return 0;
        } else if (value < _intsetGet(is,0)) {
            // 如果 value 的值小于最小值即 intset[0], 值不存在，插入的位置是 0
            if (pos) *pos = 0;
            return 0;
        }
    }

    // 这里就是二分查找的搜索代码
    while(max >= min) {
        // 获取中间下标，理解为 mid + (max - min) // 2
        mid = ((unsigned int)min + (unsigned int)max) >> 1;
        // 获取到当前 mid 下标对应的整数值
        cur = _intsetGet(is,mid);
        if (value > cur) {
            // 如果 value 大于 cur，说明要搜索的值在 cur 右边，需要去右区间搜索，收缩左边界
            min = mid+1;
        } else if (value < cur) {
            // 如果 value 小于 cur，说明要搜索的值在 cur 左边，需要去左区间搜索，收缩右边界
            max = mid-1;
        } else {
            // value == cur 说明找到了目标元素，它对应的下标就为 mid，跳出搜索
            break;
        }
    }

    if (value == cur) {
        // 如果找到了，此时是对应上面 while break 出来的，此时对应的下标位置就为 mid
        if (pos) *pos = mid;
        return 1;
    } else {
        // 如果没找到，此时是对应上面的 while 结束，即最终 min=max+1 结束循环，如果要插入 value，对应的下标就为 min
        // while 最后一次执行的总是 min=max=mid，此时 nums[mid] 左边的全部小于 value，nums[mid] 右边的全部大于 value
        // 插入位置有两种情况：
        // 1. 就是这个位置，即 nums[mid] > value 时，上面的 value < cur，此时执行了 max = mid-1，返回 min 正确
        // 2. 是这个位置的右边一个位置，即 nums[mid] < value 时，上面的 value > cur，此时执行了 min = mid+1，返回 min 正确
        if (pos) *pos = min;
        return 0;
    }
}

/* Upgrades the intset to a larger encoding and inserts the given integer. */
// 升级 intset 到更大的编码格式，并且插入对应整数值，编码升级需要针对每个整数进行，时间复杂度 O(N)
// 这个用于：发现插入元素 value 编码类型更大，此时需要先升级 intset 再进行插入
static intset *intsetUpgradeAndAdd(intset *is, int64_t value) {
    // 当前 intset 编码以及插入新元素要用的编码
    uint8_t curenc = intrev32ifbe(is->encoding);
    uint8_t newenc = _intsetValueEncoding(value);
    // intset 元素个数，即有多少个整数就要处理多少次，时间复杂度是 O(N) 的
    int length = intrev32ifbe(is->length);
    // 根据插入元素 value 是否小于 0 判断插入位置，此时只会插入到最左或者最右（因为新元素已经超出原本范围，新数据编码大于老编码）
    // value < 0 prepend == 1 在最前插入；value > 0 prepend == 0 在最后插入
    // 当然这里用到的场景是插入元素大于范围，才这么设置，对于正常的整数范围，最大要么是正数的最大，最小要么是负数的最小
    // 不然考虑一个 intset 里全都是负数（都比 -1 小）的场景，然后插入 -1，此时插入位置应该是在最后而不是最左（场景不同）
    int prepend = value < 0 ? 1 : 0;

    /* First set new encoding and resize */
    // 设置 inset 新编码，并且根据新编码加新长度进行 zrealloc 扩容
    is->encoding = intrev32ifbe(newenc);
    is = intsetResize(is,intrev32ifbe(is->length)+1);

    /* Upgrade back-to-front so we don't overwrite values.
     * Note that the "prepend" variable is used to make sure we have an empty
     * space at either the beginning or the end of the intset. */
    // 从后到前对 intset->contents 里所有的整数值进行升级，从后到前是为了不覆盖内存里的数据（新分配的内存都在数组的后端）
    // 因为数据编码类型改变了，每一个整数都需要按照新编码类型，重新进行编码然后设置到内存中
    // 升级其实就是根据当前编码取出最后一个元素，然后用新编码插入到扩容后的 contents 对应位置上
    // prepend 用来做偏移，确保我们在 intset 的开头或者结尾能留一个空白空间，用于插入新元素
    // 此时 intset 是已经完成了扩容 +1 的，进行 while(length--) 从后往前进行整数升级
    // ============================================================================================
    // 假设有下面三个元素，目前是按照 curenc 编码，它们在数组中排列如下，假如要在最后插入 6，此时 prepend = 0
    // | 3 | 4 | 5 |
    // 当对 intset 进行 resize 扩容后，数组变成下面这样，下面的 ? 代表没使用的内存
    // 假设扩容 newenc 新编码需要的内存是两倍的 curenc，用胖的代表两倍的内存，翻倍后再预留了一个新元素的空间
    // | 3 | 4 | 5 | ? |   ?   |   ?   |
    // 此时从后往前对 intset 进行升级，用 curenc 老编码取出整数再以 newenc 新编码插入整数（预留最后一个是给新元素的）
    // | 3 | 4 | 5 | ? |   5   |   ?   |    ------- 将 5 按照新编码插入新位置（倒数第二）
    // | 3 | 4 |   4   |   5   |   ?   |    ------- 将 4 按照新编码插入新位置（倒数第三）
    // |   3   |   4   |   5   |   ?   |    ------- 将 3 按照新编码插入新位置（倒数第四）
    // 可以看到这个过程元素虽然是覆盖写的，但是不会丢数据，之后在最后面设置新元素
    // |   3   |   4   |   5   |   6   |    ------- 将 6 按照新编码插入新位置（最后一个位置）
    // ================================================================================
    // 假设要在最前面插入 2，此时 prepend = 1，扩容后的数组同样为：
    // | 3 | 4 | 5 | ? |   ?   |   ?   |
    // 此时从后往前对 intset 进行升级，用 curenc 老编码取出整数再以 newenc 新编码插入整数（新位置要 +1）
    // | 3 | 4 | 5 | ? |   ?   |   5   |    ------- 将 5 按照新编码插入新位置（倒数第一）
    // | 3 | 4 | 5 | ? |   4   |   5   |    ------- 将 4 按照新编码插入新位置（倒数第二）
    // | 3 | 4 |   3   |   4   |   5   |    ------- 将 3 按照新编码插入新位置（倒数第三）
    // 也可以看到这个过程元素虽然是覆盖写的，但是不会丢数据，之后在最前面设置新元素
    // |   2   |   3   |   4   |   5   |    ------- 将 2 按照新编码插入新位置（第一个位置）
    while(length--)
        _intsetSet(is,length+prepend,_intsetGetEncoded(is,length,curenc));

    /* Set the value at the beginning or the end. */
    // 根据 prepend 在最前面或者最后面设置新元素，见上面的例子
    if (prepend)
        _intsetSet(is,0,value);
    else
        _intsetSet(is,intrev32ifbe(is->length),value);
    // 插入新元素后设置 intset 的长度 += 1
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}

// 向前或者向后移动下标范围里的数组元素，其实就用于增加元素或者删除元素
// 将 from 对应位置往后的元素，移动到 to 位置
static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {
    void *src, *dst;
    // is->length - from 算出这个位置往后有多少个元素，即要移动的元素个数
    uint32_t bytes = intrev32ifbe(is->length)-from;
    // 获取当前的编码
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64) {
        // 获取 from 的起始地址，即移动范围的初始地址
        src = (int64_t*)is->contents+from;
        // 获取 to 的起始地址，即需要移动到的目的地初始地址
        dst = (int64_t*)is->contents+to;
        // 元素个数 * 元素大小，计算要移动的总字节数
        bytes *= sizeof(int64_t);
    } else if (encoding == INTSET_ENC_INT32) {
        src = (int32_t*)is->contents+from;
        dst = (int32_t*)is->contents+to;
        bytes *= sizeof(int32_t);
    } else {
        src = (int16_t*)is->contents+from;
        dst = (int16_t*)is->contents+to;
        bytes *= sizeof(int16_t);
    }
    // 将 src 位置往后 bytes 个字节，移动到 dst 位置
    memmove(dst,src,bytes);
}

/* Insert an integer in the intset */
// 在 intset 中插入一个整数，因为插入会涉及到扩容，地址可能会改变，所以函数返回 intset 的指针
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {
    // 获取 value 的类型
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;
    if (success) *success = 1;

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    // 如果新元素的编码大于 intset 的编码，那么说明 intset 需要进行升级，然后插入元素
    if (valenc > intrev32ifbe(is->encoding)) {
        /* This always succeeds, so we don't need to curry *success. */
        // 调用 intsetUpgradeAndAdd 方法进行 intset 编码升级、扩容和插入元素
        return intsetUpgradeAndAdd(is,value);
    } else {
        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        // 通过二分查找在有序数组中搜索 value，如果有找到，说明元素已存在，插入失败
        // 否则，在二分查找的过程中，没有找到 value 时，pos 也会被设置为元素要插入的下标
        if (intsetSearch(is,value,&pos)) {
            if (success) *success = 0;
            return is;
        }

        // 此时要进行插入，对 intset 进行扩容
        is = intsetResize(is,intrev32ifbe(is->length)+1);
        // 如果 pos 插入位置小于 intset 长度，即插在中间，那么需要将目标位置后面的元素都往后移，空出位置
        // 通过调用 intsetMoveTail 函数，将 pos 位置开始的元素，移动到 pos+1 的位置上
        // 假设现在有 [1, 3, 5] 这样的数组，需要在里面插入 2，此时 is->length 为 3，pos 为 1
        // | 1 | 3 | 5 |
        // | 1 | 3 | 5 | ? |   ------ 扩容后的数组，其中 ? 表示还没使用的内存
        // | 1 | 3 | 3 | 5 |   ------ pos < is->length，将 3 开始的内存往后移动
        // | 1 | 2 | 3 | 5 |   ------ 走后面的 _intsetSet 将 2 插入到 pos = 1 的位置上
        // 而如果 pos == is->length 就不用进行内存移动，直接在数组的最后即 is->length 位置上插入即可
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1);
    }

    // 在 pos 下标插入 value 元素，is->contents[pos] = value，并且维护 is->length 长度
    _intsetSet(is,pos,value);
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}

/* Delete integer from intset */
// 从 intset 中删除指定整数，因为删除会涉及到缩容，地址可能会改变，所以函数返回 intset 的指针
intset *intsetRemove(intset *is, int64_t value, int *success) {
    // 获取 value 的类型
    uint8_t valenc = _intsetValueEncoding(value);
    // value 在数组中的下标，如果它存在的话
    uint32_t pos;
    if (success) *success = 0;

    if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,&pos)) {
        // 如果删除整数的类型 > 当前编码类型，编码大于了显然超过范围，铁定不存在
        // 二分查找判断元素是否存在，存在才进行删除，否则显然不存在直接返回
        uint32_t len = intrev32ifbe(is->length);

        /* We know we can delete */
        // 能找到，显然能进行删除
        if (success) *success = 1;

        /* Overwrite value with tail and update length */
        // 如果 pos 插入位置小于 intset 长度，即在中间删除，直接 memmove 进行内存往前移动覆盖
        // 通过调用 intsetMoveTail 函数，将 pos+1 位置开始的元素，移动到 pos 的位置上（往前移动进行覆盖待删除元素）
        if (pos < (len-1)) intsetMoveTail(is,pos+1,pos);
        // 覆盖后，即删除了目标元素，对 intset 进行缩容，并且更新 intset 长度
        is = intsetResize(is,len-1);
        is->length = intrev32ifbe(len-1);
    }
    return is;
}

/* Determine whether a value belongs to this set */
// 判断指定 value 在 intset 中是否存在，用于实现 SISMEMBER 命令，在这里实际上是 O(log N) 的时间复杂度
uint8_t intsetFind(intset *is, int64_t value) {
    uint8_t valenc = _intsetValueEncoding(value);
    return valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,NULL);
}

/* Return random member */
// 随机返回一个整数，用于实现 SRANDMEMBER 命令，时间复杂度这里为 O(1)
int64_t intsetRandom(intset *is) {
    uint32_t len = intrev32ifbe(is->length);
    assert(len); /* avoid division by zero on corrupt intset payload. */
    // 随机数对长度取余，然后直接根据下标获取整数
    return _intsetGet(is,rand()%len);
}

/* Get the value at the given position. When this position is
 * out of range the function returns 0, when in range it returns 1. */
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {
    if (pos < intrev32ifbe(is->length)) {
        *value = _intsetGet(is,pos);
        return 1;
    }
    return 0;
}

/* Return intset length */
// 获取 intset 长度，用于实现 SCARD 命令，时间复杂度 O(1)
uint32_t intsetLen(const intset *is) {
    return intrev32ifbe(is->length);
}

/* Return intset blob size in bytes. */
size_t intsetBlobLen(intset *is) {
    return sizeof(intset)+(size_t)intrev32ifbe(is->length)*intrev32ifbe(is->encoding);
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we make sure there are no duplicate or out of order records. */
int intsetValidateIntegrity(const unsigned char *p, size_t size, int deep) {
    intset *is = (intset *)p;
    /* check that we can actually read the header. */
    if (size < sizeof(*is))
        return 0;

    uint32_t encoding = intrev32ifbe(is->encoding);

    size_t record_size;
    if (encoding == INTSET_ENC_INT64) {
        record_size = INTSET_ENC_INT64;
    } else if (encoding == INTSET_ENC_INT32) {
        record_size = INTSET_ENC_INT32;
    } else if (encoding == INTSET_ENC_INT16){
        record_size = INTSET_ENC_INT16;
    } else {
        return 0;
    }

    /* check that the size matches (all records are inside the buffer). */
    uint32_t count = intrev32ifbe(is->length);
    if (sizeof(*is) + count*record_size != size)
        return 0;

    /* check that the set is not empty. */
    if (count==0)
        return 0;

    if (!deep)
        return 1;

    /* check that there are no dup or out of order records. */
    int64_t prev = _intsetGet(is,0);
    for (uint32_t i=1; i<count; i++) {
        int64_t cur = _intsetGet(is,i);
        if (cur <= prev)
            return 0;
        prev = cur;
    }

    return 1;
}

#ifdef REDIS_TEST
#include <sys/time.h>
#include <time.h>

#if 0
static void intsetRepr(intset *is) {
    for (uint32_t i = 0; i < intrev32ifbe(is->length); i++) {
        printf("%lld\n", (uint64_t)_intsetGet(is,i));
    }
    printf("\n");
}

static void error(char *err) {
    printf("%s\n", err);
    exit(1);
}
#endif

static void ok(void) {
    printf("OK\n");
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

static intset *createSet(int bits, int size) {
    uint64_t mask = (1<<bits)-1;
    uint64_t value;
    intset *is = intsetNew();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand()*rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

static void checkConsistency(intset *is) {
    for (uint32_t i = 0; i < (intrev32ifbe(is->length)-1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t*)is->contents;
            assert(i16[i] < i16[i+1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t*)is->contents;
            assert(i32[i] < i32[i+1]);
        } else {
            int64_t *i64 = (int64_t*)is->contents;
            assert(i64[i] < i64[i+1]);
        }
    }
}

#define UNUSED(x) (void)(x)
int intsetTest(int argc, char **argv, int flags) {
    uint8_t success;
    int i;
    intset *is;
    srand(time(NULL));

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    printf("Value encodings: "); {
        assert(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(-9223372036854775808ull) ==
                    INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+9223372036854775807ull) ==
                    INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: "); {
        is = intsetNew();
        is = intsetAdd(is,5,&success); assert(success);
        is = intsetAdd(is,6,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(!success);
        ok();
        zfree(is);
    }

    printf("Large number of random adds: "); {
        uint32_t inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++) {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        assert(intrev32ifbe(is->length) == inserts);
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int16 to int32: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,65535));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int16 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int32 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Stress lookups: "); {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",
               num,size,usec()-start);
        zfree(is);
    }

    printf("Stress add+delete: "); {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            assert(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            assert(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
        zfree(is);
    }

    return 0;
}
#endif
