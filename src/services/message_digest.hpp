#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

// MD4（RFC 1320）、MD5（RFC 1321）與 SHA-1（FIPS 180-4）的 64 位元組區塊壓縮函式。
// 狀態與緩衝存放在呼叫端（例如客體的 MD5_CTX），這裡只做單一區塊的運算，分段與補位由呼叫端依各格式處理。
namespace x64arm::digest {
inline std::uint32_t little(const std::uint8_t* p){std::uint32_t v;std::memcpy(&v,p,4);return v;}
inline std::uint32_t big(const std::uint8_t* p){return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];}
inline constexpr std::array<std::uint32_t,4> md_initial{0x67452301U,0xefcdab89U,0x98badcfeU,0x10325476U};
inline constexpr std::array<std::uint32_t,5> sha1_initial{0x67452301U,0xefcdab89U,0x98badcfeU,0x10325476U,0xc3d2e1f0U};
inline void md4_block(std::array<std::uint32_t,4>& state,const std::uint8_t* block){
    std::array<std::uint32_t,16> x{};for(unsigned i=0;i<16;++i)x[i]=little(block+i*4);
    auto a=state[0],b=state[1],c=state[2],d=state[3];
    const auto f=[](std::uint32_t p,std::uint32_t q,std::uint32_t r){return (p&q)|(~p&r);};
    const auto g=[](std::uint32_t p,std::uint32_t q,std::uint32_t r){return (p&q)|(p&r)|(q&r);};
    const auto h=[](std::uint32_t p,std::uint32_t q,std::uint32_t r){return p^q^r;};
    // 每輪 16 步，四個暫存器輪替；第二、三輪分別加上 sqrt(2)、sqrt(3) 的常數。
    constexpr unsigned order2[16]{0,4,8,12,1,5,9,13,2,6,10,14,3,7,11,15},order3[16]{0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
    constexpr int shift1[4]{3,7,11,19},shift2[4]{3,5,9,13},shift3[4]{3,9,11,15};
    for(unsigned i=0;i<16;++i){const auto t=std::rotl(a+f(b,c,d)+x[i],shift1[i%4]);a=d;d=c;c=b;b=t;}
    for(unsigned i=0;i<16;++i){const auto t=std::rotl(a+g(b,c,d)+x[order2[i]]+0x5a827999U,shift2[i%4]);a=d;d=c;c=b;b=t;}
    for(unsigned i=0;i<16;++i){const auto t=std::rotl(a+h(b,c,d)+x[order3[i]]+0x6ed9eba1U,shift3[i%4]);a=d;d=c;c=b;b=t;}
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
}
inline void md5_block(std::array<std::uint32_t,4>& state,const std::uint8_t* block){
    // T[i] = floor(|sin(i+1)| × 2^32)。
    static constexpr std::uint32_t table[64]{
        0xd76aa478U,0xe8c7b756U,0x242070dbU,0xc1bdceeeU,0xf57c0fafU,0x4787c62aU,0xa8304613U,0xfd469501U,0x698098d8U,0x8b44f7afU,0xffff5bb1U,0x895cd7beU,0x6b901122U,0xfd987193U,0xa679438eU,0x49b40821U,
        0xf61e2562U,0xc040b340U,0x265e5a51U,0xe9b6c7aaU,0xd62f105dU,0x02441453U,0xd8a1e681U,0xe7d3fbc8U,0x21e1cde6U,0xc33707d6U,0xf4d50d87U,0x455a14edU,0xa9e3e905U,0xfcefa3f8U,0x676f02d9U,0x8d2a4c8aU,
        0xfffa3942U,0x8771f681U,0x6d9d6122U,0xfde5380cU,0xa4beea44U,0x4bdecfa9U,0xf6bb4b60U,0xbebfbc70U,0x289b7ec6U,0xeaa127faU,0xd4ef3085U,0x04881d05U,0xd9d4d039U,0xe6db99e5U,0x1fa27cf8U,0xc4ac5665U,
        0xf4292244U,0x432aff97U,0xab9423a7U,0xfc93a039U,0x655b59c3U,0x8f0ccc92U,0xffeff47dU,0x85845dd1U,0x6fa87e4fU,0xfe2ce6e0U,0xa3014314U,0x4e0811a1U,0xf7537e82U,0xbd3af235U,0x2ad7d2bbU,0xeb86d391U};
    static constexpr int shifts[4][4]{{7,12,17,22},{5,9,14,20},{4,11,16,23},{6,10,15,21}};
    std::array<std::uint32_t,16> x{};for(unsigned i=0;i<16;++i)x[i]=little(block+i*4);
    auto a=state[0],b=state[1],c=state[2],d=state[3];
    for(unsigned i=0;i<64;++i){
        const unsigned round=i/16;std::uint32_t mix;unsigned index;
        switch(round){
        case 0:mix=(b&c)|(~b&d);index=i;break;
        case 1:mix=(b&d)|(c&~d);index=(5*i+1)%16;break;
        case 2:mix=b^c^d;index=(3*i+5)%16;break;
        default:mix=c^(b|~d);index=(7*i)%16;break;}
        const auto t=b+std::rotl(a+mix+x[index]+table[i],shifts[round][i%4]);a=d;d=c;c=b;b=t;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
}
inline void sha1_block(std::array<std::uint32_t,5>& state,const std::uint8_t* block){
    std::array<std::uint32_t,80> w{};for(unsigned i=0;i<16;++i)w[i]=big(block+i*4);
    for(unsigned i=16;i<80;++i)w[i]=std::rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    auto a=state[0],b=state[1],c=state[2],d=state[3],e=state[4];
    for(unsigned i=0;i<80;++i){
        std::uint32_t f,k;
        if(i<20){f=(b&c)|(~b&d);k=0x5a827999U;}else if(i<40){f=b^c^d;k=0x6ed9eba1U;}else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdcU;}else{f=b^c^d;k=0xca62c1d6U;}
        const auto t=std::rotl(a,5)+f+e+k+w[i];e=d;d=c;c=std::rotl(b,30);b=a;a=t;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;
}
}
