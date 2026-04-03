/*
 * Minimal Hedgehog-like embedding backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_HEDGEHOG_BACKEND_H
#define SYSTEM_HEDGEHOG_BACKEND_H

#include "qapi/error.h"
#include "exec/hwaddr.h"
#include "exec/vaddr.h"
#include "exec/memattrs.h"
#include "system/memory.h"
#include <stddef.h>

typedef struct CPUState CPUState;
typedef struct AddressSpace AddressSpace;
typedef struct HedgehogBackend HedgehogBackend;

typedef uint64_t (*HedgehogMMIOReadFunc)(void *opaque, hwaddr addr,
                                        unsigned size);
typedef void (*HedgehogMMIOWriteFunc)(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size);
typedef bool (*HedgehogExecHookFunc)(HedgehogBackend *uc, vaddr pc,
                                    void *opaque);

typedef enum HedgehogMemAccessType {
    HEDGEHOG_MEM_ACCESS_READ = 0,
    HEDGEHOG_MEM_ACCESS_WRITE,
    HEDGEHOG_MEM_ACCESS_FETCH,
} HedgehogMemAccessType;

typedef bool (*HedgehogInvalidMemHookFunc)(HedgehogBackend *uc,
                                          vaddr addr,
                                          unsigned size,
                                          HedgehogMemAccessType access_type,
                                          MemTxResult response,
                                          void *opaque);

typedef enum HedgehogRunResult {
    HEDGEHOG_RUN_BUDGET_EXHAUSTED = 0,
    HEDGEHOG_RUN_STOP_REQUESTED,
    HEDGEHOG_RUN_HALTED,
    HEDGEHOG_RUN_EXCEPTION,
    HEDGEHOG_RUN_INVALID_MEMORY,
} HedgehogRunResult;

bool hedgehog_backend_initialize(Error **errp);
HedgehogBackend *hedgehog_backend_new(const char *cpu_type, Error **errp);
void hedgehog_backend_free(HedgehogBackend *uc);

bool hedgehog_backend_map_ram(HedgehogBackend *uc, const char *name,
                             hwaddr addr, uint64_t size, Error **errp);
bool hedgehog_backend_map_mmio(HedgehogBackend *uc, const char *name,
                              hwaddr addr, uint64_t size,
                              HedgehogMMIOReadFunc read_fn,
                              HedgehogMMIOWriteFunc write_fn,
                              void *opaque, Error **errp);

MemTxResult hedgehog_backend_mem_read(HedgehogBackend *uc, hwaddr addr,
                                     void *buf, hwaddr len);
MemTxResult hedgehog_backend_mem_write(HedgehogBackend *uc, hwaddr addr,
                                      const void *buf, hwaddr len);

int hedgehog_backend_reg_read(HedgehogBackend *uc, int regno,
                             uint8_t *buf, size_t buf_size,
                             Error **errp);
int hedgehog_backend_reg_write(HedgehogBackend *uc, int regno,
                              const uint8_t *buf, size_t buf_size,
                              Error **errp);

void hedgehog_backend_set_tb_hook(HedgehogBackend *uc,
                                 HedgehogExecHookFunc hook_fn,
                                 void *opaque);
void hedgehog_backend_set_insn_hook(HedgehogBackend *uc,
                                   HedgehogExecHookFunc hook_fn,
                                   void *opaque);
void hedgehog_backend_set_invalid_mem_hook(HedgehogBackend *uc,
                                          HedgehogInvalidMemHookFunc hook_fn,
                                          void *opaque);

void hedgehog_backend_reset(HedgehogBackend *uc);
void hedgehog_backend_set_pc(HedgehogBackend *uc, vaddr addr);
vaddr hedgehog_backend_get_pc(HedgehogBackend *uc);

HedgehogRunResult hedgehog_backend_run(HedgehogBackend *uc,
                                     uint64_t max_instructions,
                                     int *cpu_exit);
void hedgehog_backend_stop(HedgehogBackend *uc);

CPUState *hedgehog_backend_cpu(HedgehogBackend *uc);
AddressSpace *hedgehog_backend_address_space(HedgehogBackend *uc);

#endif
