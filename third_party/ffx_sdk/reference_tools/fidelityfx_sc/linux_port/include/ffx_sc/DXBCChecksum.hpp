#pragma once

#include <cstdint>

using BYTE  = uint8_t;
using DWORD = uint32_t;
using UINT  = uint32_t;
using BOOL  = bool;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

BOOL CalculateDXBCChecksum(BYTE* pData, DWORD dwSize, DWORD dwHash[4]);

