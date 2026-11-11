/*
 * viogpu on a Gunyah protected VM (DroidVM).
 *
 * In a protected VM the host can only read and write the restricted DMA pool (rdmapool.sys,
 * ACPI\RDMA0000); ordinary guest pages are lent to the guest and invisible to crosvm. So every
 * byte the virtio-gpu device touches has to live in that pool:
 *   - the vrings              -> a ring area, bump-allocated (mem_alloc_contiguous_pages)
 *   - commands and responses  -> one pool page per in-flight buffer ("slot"), copied in on
 *                                queue and the response copied back on dequeue
 *   - framebuffer and cursor  -> their memory segments come straight from the pool, as one
 *                                contiguous region (a single backing entry)
 * Without rdmapool (QEMU/KVM) nothing here is active and the stock paths run unchanged.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

extern "C"
{
#include <ntddk.h>
}

/* Slot layout: [command 0..1023][data 1024..2047][response 2048..4095]. */
#define VIOGPU_RDMA_SLOT_CMD_OFF  0
#define VIOGPU_RDMA_SLOT_DATA_OFF 1024
#define VIOGPU_RDMA_SLOT_RESP_OFF 2048
#define VIOGPU_RDMA_SLOT_CMD_MAX  1024
#define VIOGPU_RDMA_SLOT_DATA_MAX 1024
#define VIOGPU_RDMA_SLOT_RESP_MAX 2048

/* Open the pool (PASSIVE_LEVEL, idempotent). FALSE: no pool, stock behaviour. */
BOOLEAN VioGpuRdmaOpen(void);
BOOLEAN VioGpuRdmaActive(void);
/* Start of every adapter init (PASSIVE_LEVEL): rewinds the ring area and refills the slots. */
void VioGpuRdmaReset(void);

/* Vring memory (PASSIVE_LEVEL). NULL when the ring area is used up. */
PVOID VioGpuRdmaRingAlloc(SIZE_T size);
/* TRUE when va lies in any pool region this driver holds; *pa is then its device address. */
BOOLEAN VioGpuRdmaVaToPa(PVOID va, PHYSICAL_ADDRESS *pa);

/* One PAGE_SIZE slot, any IRQL. NULL when all slots are in flight. */
PVOID VioGpuRdmaSlotAlloc(void);
void VioGpuRdmaSlotFree(PVOID slot);

/* A contiguous pool region (PASSIVE_LEVEL), for the frame and cursor segments. */
PVOID VioGpuRdmaRegionAlloc(SIZE_T bytes, PHYSICAL_ADDRESS *pa);
void VioGpuRdmaRegionFree(PVOID va);
