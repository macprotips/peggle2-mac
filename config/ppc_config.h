#pragma once
// Image layout of the retail Default.xex (as XenonRecomp writes it), for building the runtime on its own.
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED


#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0xDF0000ull
#define PPC_CODE_BASE 0x82140000ull
#define PPC_CODE_SIZE 0x76F6A8ull

#ifdef PPC_INCLUDE_DETAIL
#include "ppc_detail.h"
#endif

#endif
