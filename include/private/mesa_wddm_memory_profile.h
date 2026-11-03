/* SPDX-License-Identifier: MIT */
#ifndef MESA_WDDM_MEMORY_PROFILE_H
#define MESA_WDDM_MEMORY_PROFILE_H
#include <stdint.h>

/* Independent output-only pNext contract. This does not extend runtime ABI1/2.
 * Zero-initialize the payload before querying; zero magic means unavailable. */
#define MWD_STYPE_MEMORY_PROFILE_V1 0x7fff2339
#define MWD_MEMORY_PROFILE_MAGIC 0x31504d57u
#define MWD_MEMORY_PROFILE_VERSION 1u
#define MWD_MEMORY_PROFILE_OWNED_RUNTIME2 1u
#define MWD_MEMORY_RETAINS_OWNED_BACKING 1u
#define MWD_MEMORY_CACHED_COHERENT_AVAILABLE 2u
#define MWD_MEMORY_KNOWN_MASK 3u

struct mwd_memory_profile_v1 {
   int32_t sType;
   void *pNext;
   uint32_t magic, version, size, profile;
   uint8_t luid[8];
   uint64_t reset_generation;
   uint32_t known_mask, value_mask;
   uint32_t reserved[2];
};
#endif
