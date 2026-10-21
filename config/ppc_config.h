#pragma once
// XenonRecomp's ppc_config.h for the retail Default.xex, for building the runtime on its own. The options
// shape PPCContext, so they must match the generated one (tools/regen.sh checks).
#ifndef PPC_CONFIG_H_INCLUDED
#define PPC_CONFIG_H_INCLUDED

#define PPC_CONFIG_CTR_AS_LOCAL
#define PPC_CONFIG_XER_AS_LOCAL
#define PPC_CONFIG_RESERVED_AS_LOCAL
#define PPC_CONFIG_CR_AS_LOCAL

#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0xDF0000ull
#define PPC_CODE_BASE 0x82140000ull
#define PPC_CODE_SIZE 0x76F6A8ull

#ifdef PPC_INCLUDE_DETAIL
#include "ppc_detail.h"
#endif

#endif
