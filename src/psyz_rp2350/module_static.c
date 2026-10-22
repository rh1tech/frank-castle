/*
 * frank-castle — psyz module host for statically linked overlays.
 *
 * Every overlay is linked into the firmware with only two global symbols,
 * <name>__module_start/stop. Its code and rodata run from flash; its .data and
 * .bss live in a PSRAM slot shared by all overlays of the same kind, so opening
 * a module copies its initial data image from flash into the slot -- what the
 * PS1 did when it read an overlay from disc.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "module_static.h"

#include <psyz/log.h>
#include <psyz/module.h>
#include <string.h>

static int g_SlotOwner[MODULE_SLOT_COUNT] = {-1, -1, -1, -1};

static const StaticModule *Find(const char *name) {
    for (unsigned i = 0; i < g_StaticModuleCount; i++)
        if (strcmp(g_StaticModules[i].name, name) == 0) return &g_StaticModules[i];
    return NULL;
}

PsyzModule Psyz_ModuleOpen(const char *name, void *param) {
    const StaticModule *m = Find(name);
    if (!m) {
        ERRORF("module '%s' is not linked into this firmware", name);
        return 0;
    }
    int idx = (int)(m - g_StaticModules);
    int owner = g_SlotOwner[m->slot];
    if (owner >= 0 && owner != idx) {
        ERRORF("module '%s': slot %d still holds '%s'", name, m->slot, g_StaticModules[owner].name);
        return 0;
    }
    memcpy(m->vma, m->load_start, (size_t)(m->load_end - m->load_start));
    g_SlotOwner[m->slot] = idx;
    m->start(param);
    return (PsyzModule)(idx + 1);
}

void Psyz_ModuleClose(PsyzModule descriptor) {
    if (!descriptor || descriptor > g_StaticModuleCount) return;
    int idx = (int)descriptor - 1;
    const StaticModule *m = &g_StaticModules[idx];
    if (g_SlotOwner[m->slot] != idx) {
        WARNF("module '%s' is not loaded", m->name);
        return;
    }
    m->stop();
    g_SlotOwner[m->slot] = -1;
}
