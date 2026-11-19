#pragma once
/* IUnknown_GetWindow 的查詢順序：IOleWindow、IShellView、IInternetSecurityMgrSite。 */
static const unsigned char shell_window_iids[][16]={
    {0x14,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},
    {0xe3,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},
    {0xed,0xc9,0xea,0x79,0xf9,0xba,0xce,0x11,0x8c,0x82,0,0xaa,0,0x4b,0xa9,0x0b}
};
static const unsigned char shell_service_iids[][16]={{
    0,0xfb,0x36,0x58,0x87,0x81,0xcf,0x11,0xa1,0x2b,0,0xaa,0,0x4a,0xe8,0x37
}};

/* IPersist、IPersistFolder 都在第 3 格提供 GetClassID。 */
static const unsigned char shell_persist_iids[][16]={
    {0x0c,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},
    {0xea,0x14,0x02,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46}
};
struct ShellInterfaceQuery {
    const unsigned char (*iids)[16];
    unsigned count;
    int require_s_ok;
};
static const struct ShellInterfaceQuery window_query={shell_window_iids,3,0};
static const struct ShellInterfaceQuery owner_query={shell_service_iids,1,1};
static const struct ShellInterfaceQuery class_id_query={shell_persist_iids,2,1};
