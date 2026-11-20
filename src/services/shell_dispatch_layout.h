#pragma once

/* 參考實作 SHPackDispParams 只辨識這些 VARTYPE；其餘以 VT_I4 傳遞。 */
static inline unsigned short shell_dispatch_type(unsigned short type)
{
    if ((type & 0x4000) || type == 8 || type == 9 || type == 11 || type == 13)
        return type;
    return 3;
}

static inline unsigned shell_dispatch_width(unsigned short type, unsigned pointer_bits)
{
    if (type == 11) return 16;
    if (type == 3) return 32;
    return pointer_bits;
}
