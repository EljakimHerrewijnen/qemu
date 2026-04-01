/*
 * Minimal Unicorn-like embedding backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_UNICORN_BACKEND_H
#define SYSTEM_UNICORN_BACKEND_H

#include "qapi/error.h"
#include "exec/hwaddr.h"
#include "exec/vaddr.h"
#include "exec/memattrs.h"
#include "system/memory.h"

typedef struct CPUState CPUState;
typedef struct AddressSpace AddressSpace;
typedef struct UnicornBackend UnicornBackend;

typedef uint64_t (*UnicornMMIOReadFunc)(void *opaque, hwaddr addr,
                                        unsigned size);
typedef void (*UnicornMMIOWriteFunc)(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size);

typedef enum UnicornRunResult {
    UNICORN_RUN_BUDGET_EXHAUSTED = 0,
    UNICORN_RUN_STOP_REQUESTED,
    UNICORN_RUN_HALTED,
    UNICORN_RUN_EXCEPTION,
} UnicornRunResult;

bool unicorn_backend_initialize(Error **errp);
UnicornBackend *unicorn_backend_new(const char *cpu_type, Error **errp);
void unicorn_backend_free(UnicornBackend *uc);

bool unicorn_backend_map_ram(UnicornBackend *uc, const char *name,
                             hwaddr addr, uint64_t size, Error **errp);
bool unicorn_backend_map_mmio(UnicornBackend *uc, const char *name,
                              hwaddr addr, uint64_t size,
                              UnicornMMIOReadFunc read_fn,
                              UnicornMMIOWriteFunc write_fn,
                              void *opaque, Error **errp);

MemTxResult unicorn_backend_mem_read(UnicornBackend *uc, hwaddr addr,
                                     void *buf, hwaddr len);
MemTxResult unicorn_backend_mem_write(UnicornBackend *uc, hwaddr addr,
                                      const void *buf, hwaddr len);

void unicorn_backend_reset(UnicornBackend *uc);
void unicorn_backend_set_pc(UnicornBackend *uc, vaddr addr);
vaddr unicorn_backend_get_pc(UnicornBackend *uc);

UnicornRunResult unicorn_backend_run(UnicornBackend *uc,
                                     uint64_t max_instructions,
                                     int *cpu_exit);
void unicorn_backend_stop(UnicornBackend *uc);

CPUState *unicorn_backend_cpu(UnicornBackend *uc);
AddressSpace *unicorn_backend_address_space(UnicornBackend *uc);

#endif
