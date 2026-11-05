/* Windows LLP64 數值極限；M/N/O 分別為 float/double/64-bit long double。
 * 欄位：型別碼、C 型別、位元數、浮點、帶正負號、digits、digits10、min、max、epsilon、round_error、infinity、quiet_NaN、signaling_NaN、denorm_min。 */
#define MSVCP_NUMERIC_TYPES(APPLY) \
 APPLY(C,signed char,8,0,1,7,2,0xffffffffffffff80ULL,0x7f,0,0,0,0,0,0) \
 APPLY(D,signed char,8,0,1,7,2,0xffffffffffffff80ULL,0x7f,0,0,0,0,0,0) \
 APPLY(E,unsigned char,8,0,0,8,2,0,0xff,0,0,0,0,0,0) \
 APPLY(F,short,16,0,1,15,4,0xffffffffffff8000ULL,0x7fff,0,0,0,0,0,0) \
 APPLY(G,unsigned short,16,0,0,16,4,0,0xffff,0,0,0,0,0,0) \
 APPLY(H,int,32,0,1,31,9,0xffffffff80000000ULL,0x7fffffff,0,0,0,0,0,0) \
 APPLY(I,unsigned int,32,0,0,32,9,0,0xffffffff,0,0,0,0,0,0) \
 APPLY(J,long,32,0,1,31,9,0xffffffff80000000ULL,0x7fffffff,0,0,0,0,0,0) \
 APPLY(K,unsigned long,32,0,0,32,9,0,0xffffffff,0,0,0,0,0,0) \
 APPLY(M,float,32,1,1,24,6,0x00800000,0x7f7fffff,0x34000000,0x3f000000,0x7f800000,0x7fc00000,0x7f800001,1) \
 APPLY(N,double,64,1,1,53,15,0x0010000000000000ULL,0x7fefffffffffffffULL,0x3cb0000000000000ULL,0x3fe0000000000000ULL,0x7ff0000000000000ULL,0x7ff8000000000000ULL,0x7ff0000000000001ULL,1) \
 APPLY(O,double,64,1,1,53,15,0x0010000000000000ULL,0x7fefffffffffffffULL,0x3cb0000000000000ULL,0x3fe0000000000000ULL,0x7ff0000000000000ULL,0x7ff8000000000000ULL,0x7ff0000000000001ULL,1) \
 APPLY(_J,long long,64,0,1,63,18,0x8000000000000000ULL,0x7fffffffffffffffULL,0,0,0,0,0,0) \
 APPLY(_K,unsigned long long,64,0,0,64,19,0,0xffffffffffffffffULL,0,0,0,0,0,0) \
 APPLY(_N,unsigned char,8,0,0,1,0,0,1,0,0,0,0,0,0) \
 APPLY(_W,unsigned short,16,0,0,16,4,0,0xffff,0,0,0,0,0,0)
