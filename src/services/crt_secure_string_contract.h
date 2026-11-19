#pragma once
/* 安全字串家族共用的元素數契約；容量不是實際讀寫或配置的大小。 */
static inline unsigned long long x64arm_crt_secure_rsize_max(unsigned pointer_bytes){
    return pointer_bytes==4?0x7fffffffULL:0x7fffffffffffffffULL;
}
static inline unsigned long long x64arm_crt_secure_scan_limit(unsigned long long capacity,
        unsigned long long used,unsigned long long count,unsigned long long truncate_value,unsigned bounded){
    const unsigned long long remaining=capacity-used;
    return bounded && count!=truncate_value && count<remaining?count:remaining;
}
