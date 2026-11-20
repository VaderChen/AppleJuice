#pragma once
#include "com_agile_layout.h"
/* AddRef／Release 共用既有的引用計數與銷毀回呼欄位。 */
#define SHELL_FACTORY_CALLBACK 16
#define SHELL_FACTORY_DLL_COUNT 32
#define SHELL_FACTORY_FILTER 40
#define SHELL_FACTORY_VTABLE 48
static const unsigned char shell_factory_iid[16]={1,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
