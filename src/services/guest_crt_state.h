#pragma once
/* TLS 擴充槽佔滿 0x2000–0x4000；CRT 私有頁接在其後，與 FLS／例外資料分離。
 * 分類與大小寫表複製到穩定的執行緒地址：另一執行緒 setlocale 釋放舊物件時，
 * 已載入暫存器的表格指標仍有效。只在執行緒建立及 locale 改變時更新。 */
#define GUEST_CRT_TEB_SIZE 0x5000
#define GUEST_CRT_STATE_OFFSET 0x4000
#define GUEST_CRT_TABLE_OFFSET 0x4100
#define GUEST_CRT_ERRNO_OFFSET 0x4ff0
typedef struct GuestCrtThreadState {
    unsigned long long errno_address;
    unsigned long long locale_address;
    unsigned long long ctype_address;
    unsigned long long lower_address;
    unsigned long long upper_address;
    unsigned ctype_codepage;
    unsigned numeric_codepage;
} GuestCrtThreadState;
