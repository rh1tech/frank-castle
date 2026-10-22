/*
 * frank-castle — statically linked psyz modules (PS1-style overlays).
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef MODULE_STATIC_H
#define MODULE_STATIC_H

#include <stdint.h>

enum {
    MODULE_SLOT_STAGE,
    MODULE_SLOT_WEAPON0,
    MODULE_SLOT_WEAPON1,
    MODULE_SLOT_SERVANT,
    MODULE_SLOT_COUNT,
};

typedef struct {
    const char *name;
    uint8_t slot;
    void (*start)(void *param);
    void (*stop)(void);
    const uint8_t *load_start; /* .data+.bss image in flash */
    const uint8_t *load_end;
    uint8_t *vma;              /* the slot in PSRAM */
} StaticModule;

extern const StaticModule g_StaticModules[];
extern const unsigned g_StaticModuleCount;

#endif
