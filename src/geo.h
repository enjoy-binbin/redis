#ifndef __GEO_H__
#define __GEO_H__

#include "server.h"

/* Structures used inside geo.c in order to represent points and array of
 * points on the earth. */
typedef struct geoPoint {
    double longitude; // 经度
    double latitude; // 纬度
    double dist; // 这个经纬度与另一个点之间的距离
    double score; // 解码出经纬度的分值
    char *member; // 分值对应的有序集合成员
} geoPoint;

typedef struct geoArray {
    struct geoPoint *array; // 用于储存多个地理位置的数组
    size_t buckets; // 数组可用的项数量
    size_t used; // 数组目前已用的项数量
} geoArray;

#endif
