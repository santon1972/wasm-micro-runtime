/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_loader.h"
#include "bh_log.h"
#include "bh_platform.h" /* For os_printf, TEMPLATE_READ_VALUE */
#include "wasm_loader.h" /* For wasm_loader_load, wasm_loader_unload, LoadArgs */
#include "wasm_loader_common.h" /* For read_leb, WASMSection, etc. */
#include "../common/wasm_component.h"
#include "../common/wasm_c_api_internal.h" /* For loader_malloc, wasm_runtime_free */


/* Adapted from wasm_loader.c */
static void
destroy_sections(WASMSection *section_list)
{
    WASMSection *section = section_list, *next;
    while (section) {
        next = section->next;
        wasm_runtime_free(section);
        section = next;
    }
}

/* Adapted from wasm_loader.c for creating sections from a core module's data buffer */
static bool
create_sections_from_core_module_data(const uint8 *buf, uint32 size,
                                      WASMSection **p_section_list,
                                      char *error_buf, uint32 error_buf_size)
{
    WASMSection *section_list_end = NULL, *section;
    const uint8 *p = buf, *p_end = buf + size;
    uint8 section_type;
    uint32 section_size_from_leb; /* Renamed to avoid conflict */

    bh_assert(!*p_section_list);

    /* Skip WASM magic and version, as they are part of the module_data */
    /* but not part of the section list itself */
    if (size < 8) { /* Basic check for magic + version */
        set_error_buf(error_buf, error_buf_size, "core module data too short");
        return false;
    }
    p += 8;

    while (p < p_end) {
        if ((uintptr_t)p + 1 > (uintptr_t)p_end) { /* Check for reading section_type */
             set_error_buf(error_buf, error_buf_size, "unexpected end when reading section type");
             goto fail;
        }
        section_type = *p++;

        if (!read_leb((uint8 **)&p, p_end, 32, false, (uint64*)&section_size_from_leb,
                      error_buf, error_buf_size)) {
            goto fail;
        }

        if ((uintptr_t)p + section_size_from_leb < (uintptr_t)p
            || (uintptr_t)p + section_size_from_leb > (uintptr_t)p_end) {
            set_error_buf(error_buf, error_buf_size, "section size out of bounds");
            goto fail;
        }

        if (!(section = loader_malloc(sizeof(WASMSection), error_buf,
                                      error_buf_size))) {
            goto fail;
        }
        memset(section, 0, sizeof(WASMSection));

        section->section_type = section_type;
        section->section_body = (uint8 *)p;
        section->section_body_size = section_size_from_leb;

        if (!section_list_end)
            *p_section_list = section_list_end = section;
        else {
            section_list_end->next = section;
            section_list_end = section;
        }
        p += section_size_from_leb;
    }

    if (p != p_end) {
        set_error_buf(error_buf, error_buf_size, "section size mismatch after parsing all sections");
        goto fail;
    }

    return true;
fail:
    if (*p_section_list) {
        destroy_sections(*p_section_list);
        *p_section_list = NULL;
    }
    return false;
}


static void
set_error_buf(char *error_buf, uint32 error_buf_size, const char *string)
{
    if (error_buf != NULL) {
        snprintf(error_buf, error_buf_size, "WASM component load failed: %s",
                 string);
    }
}

static void
set_error_buf_v(char *error_buf, uint32 error_buf_size, const char *format, ...)
{
    va_list args;
    char buf[128];

    if (error_buf != NULL) {
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        snprintf(error_buf, error_buf_size, "WASM component load failed: %s", buf);
    }
}

static bool
check_buf(const uint8 *buf, const uint8 *buf_end, uint32 length,
          char *error_buf, uint32 error_buf_size)
{
    if ((uintptr_t)buf + length < (uintptr_t)buf
        || (uintptr_t)buf + length > (uintptr_t)buf_end) {
        set_error_buf(error_buf, error_buf_size, "unexpected end of section");
        return false;
    }
    return true;
}

#define CHECK_BUF(buf, buf_end, length)                                    \
    do {                                                                   \
        if (!check_buf(buf, buf_end, length, error_buf, error_buf_size)) { \
            goto fail;                                                     \
        }                                                                  \
    } while (0)

#define skip_leb(p) while (*p++ & 0x80)

#define read_leb_uint32(p, p_end, res)                                   \
    do {                                                                 \
        uint64 res64;                                                    \
        if (!read_leb((uint8 **)&p, p_end, 32, false, &res64, error_buf, \
                      error_buf_size))                                   \
            goto fail;                                                   \
        res = (uint32)res64;                                             \
    } while (0)


static bool
load_core_module_section(const uint8 **p_buf, const uint8 *buf_end,
                         WASMComponent *component,
                         char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    /* section_end here refers to the end of the Core Module Section itself,
       not the end of the whole component buffer. */
    const uint8 *section_content_end = p; /* Will be updated after reading module_count */
    uint32 module_count, i;

    read_leb_uint32(p, *buf_end, module_count);
    LOG_VERBOSE("Component Core Module section with %u modules found.", module_count);

    section_content_end = p; /* p is now after reading module_count */

    if (module_count == 0) {
        *p_buf = p;
        return true;
    }

    if (component->core_modules) {
        set_error_buf(error_buf, error_buf_size, "duplicate core module section");
        goto fail;
    }

    component->core_modules = loader_malloc(
        module_count * sizeof(WASMComponentCoreModule), error_buf, error_buf_size);
    if (!component->core_modules) {
        goto fail;
    }
    component->core_module_count = module_count;
    memset(component->core_modules, 0, module_count * sizeof(WASMComponentCoreModule));

    for (i = 0; i < module_count; i++) {
        uint32 module_len;
        WASMSection *core_module_sections = NULL;
        WASMComponentCoreModule *core_module_entry = &component->core_modules[i];

        /* Update section_content_end for each module's data block */
        /* This assumes module_len is relative to the current position of p */
        read_leb_uint32(p, *buf_end, module_len);
        core_module_entry->module_len = module_len;
        CHECK_BUF(p, *buf_end, module_len);
        core_module_entry->module_data = (uint8*)p;

        LOG_VERBOSE("Parsing core module %u, length %u.", i, module_len);

        /* Create WASMSection list from the core module's data */
        if (!create_sections_from_core_module_data(
                core_module_entry->module_data, core_module_entry->module_len,
                &core_module_sections, error_buf, error_buf_size)) {
            /* Error message is set by create_sections_from_core_module_data */
            goto fail;
        }

        /* Load the core module from the created sections */
        core_module_entry->module_object = wasm_loader_load_from_sections(
            core_module_sections, error_buf, error_buf_size);

        if (core_module_sections) {
            destroy_sections(core_module_sections);
            core_module_sections = NULL;
        }

        if (!core_module_entry->module_object) {
            /* Error message is already set by wasm_loader_load_from_sections */
            goto fail;
        }
        LOG_VERBOSE("Core module %u loaded successfully via sections.", i);
        p += module_len;
    }

    *p_buf = p;
    return true;

fail:
    /* component->core_modules will be freed in wasm_component_unload if allocated */
    return false;
}

static bool
load_core_instance_section(const uint8 **p_buf, const uint8 *buf_end,
                           WASMComponent *component,
                           char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Core Instance section found.\n");
    return true;
fail:
    return false;
}

static bool
load_core_type_section(const uint8 **p_buf, const uint8 *buf_end,
                       WASMComponent *component,
                       char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Core Type section found.\n");
    return true;
fail:
    return false;
}

static bool
load_component_section(const uint8 **p_buf, const uint8 *buf_end,
                       WASMComponent *component,
                       char *error_buf, uint32 error_buf_size)
{
    os_printf("Component (nested) Component section found.\n");
    return true;
fail:
    return false;
}

static bool
load_instance_section(const uint8 **p_buf, const uint8 *buf_end,
                      WASMComponent *component,
                      char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Instance section found.\n");
    return true;
fail:
    return false;
}

static bool
load_alias_section(const uint8 **p_buf, const uint8 *buf_end,
                   WASMComponent *component,
                   char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Alias section found.\n");
    return true;
fail:
    return false;
}

static bool
load_type_section(const uint8 **p_buf, const uint8 *buf_end,
                  WASMComponent *component,
                  char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Type section found.\n"); /* This is Section ID 6 */
    /* For Subtask 4, we are interested in Component Type Section ID 7 (deftype) */
    /* This function (for Section ID 6) will remain a stub for now. */
    return true;
fail:
    return false;
}

static bool
load_canonical_section(const uint8 **p_buf, const uint8 *buf_end,
                       WASMComponent *component,
                       char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Canonical section found.\n");
    return true;
fail:
    return false;
}

static bool
load_start_section(const uint8 **p_buf, const uint8 *buf_end,
                   WASMComponent *component,
                   char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Start section found.\n");
    return true;
fail:
    return false;
}

static bool
load_import_section(const uint8 **p_buf, const uint8 *buf_end,
                    WASMComponent *component,
                    char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Import section found.\n");
    return true;
fail:
    return false;
}

static bool
load_export_section(const uint8 **p_buf, const uint8 *buf_end,
                    WASMComponent *component,
                    char *error_buf, uint32 error_buf_size)
{
    os_printf("Component Export section found.\n");
    return true;
fail:
    return false;
}

WASMComponent*
wasm_component_load(const uint8 *buf, uint32 size,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = buf, *p_end = buf + size;
    uint32 magic_number, version, section_id, section_size;
    uint16 layer;
    WASMComponent *component = NULL;

    /* Check magic number */
    if ((uintptr_t)p + sizeof(uint32) > (uintptr_t)p_end
        || (*(uint32*)p != WASM_MAGIC_NUMBER
            && *(uint32*)p != CORE_WASM_MAGIC_NUMBER_PRIMARY)) {
        set_error_buf(error_buf, error_buf_size, "magic header not detected");
        return NULL;
    }
    p += sizeof(uint32);

    /* Check version */
    if ((uintptr_t)p + sizeof(uint32) > (uintptr_t)p_end
        || (*(uint32*)p != COMPONENT_MODEL_VERSION_0D
            && *(uint32*)p != COMPONENT_MODEL_VERSION_PRIMARY)) {
        set_error_buf(error_buf, error_buf_size, "unknown component binary version");
        return NULL;
    }
    version = *(uint32*)p;
    p += sizeof(uint32);

    /* Check layer */
    if ((uintptr_t)p + sizeof(uint16) > (uintptr_t)p_end
        || (*(uint16*)p != COMPONENT_MODEL_LAYER_01
            && *(uint16*)p != COMPONENT_MODEL_LAYER_PRIMARY)) {
        set_error_buf(error_buf, error_buf_size, "unknown component layer version");
        return NULL;
    }
    layer = *(uint16*)p;
    p += sizeof(uint16);

    /* Allocate memory for the component structure */
    component = loader_malloc(sizeof(WASMComponent), error_buf, error_buf_size);
    if (!component) {
        return NULL;
    }
    component->version = version;
    component->layer = layer;

    os_printf("WASM Component Magic, Version, Layer verified.\n");

    /* Iterate over sections */
    while (p < p_end) {
        read_leb_uint32(p, p_end, section_id);
        read_leb_uint32(p, p_end, section_size);

        CHECK_BUF(p, p_end, section_size);
        const uint8 *section_start = p;

        switch (section_id) {
            case COMPONENT_SECTION_ID_CORE_MODULE:
                if (!load_core_module_section(&p, p + section_size, component,
                                              error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_CORE_INSTANCE:
                if (!load_core_instance_section(&p, p + section_size, component,
                                                error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_CORE_TYPE:
                 if (!load_core_type_section(&p, p + section_size, component,
                                            error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_COMPONENT:
                if (!load_component_section(&p, p + section_size, component,
                                            error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_INSTANCE:
                if (!load_instance_section(&p, p + section_size, component,
                                           error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_ALIAS:
                if (!load_alias_section(&p, p + section_size, component,
                                        error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_TYPE: /* This is Section ID 6 */
                if (!load_type_section(&p, p + section_size, component, /* This is for Section ID 6 */
                                       error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_CANONICAL:
                if (!load_canonical_section(&p, p + section_size, component,
                                            error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_START:
                if (!load_start_section(&p, p + section_size, component,
                                        error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_IMPORT:
                if (!load_import_section(&p, p + section_size, component,
                                         error_buf, error_buf_size))
                    goto fail;
                break;
            case COMPONENT_SECTION_ID_EXPORT:
                if (!load_export_section(&p, p + section_size, component,
                                         error_buf, error_buf_size))
                    goto fail;
                break;
            default:
                 /* Check if it's the Component Type Section (ID 7) for deftypes */
                 /* This is a bit of a hack as section IDs can be anything for custom sections */
                 /* but the spec usually lists known sections first. */
                 /* A better approach would be to have a distinct ID for Component Type Section in wasm_component_loader.h */
                 /* For now, let's assume if it's not any of the above, and if we want to handle deftype section, */
                 /* we might need a way to identify it, or it's implicitly handled by the Type Section (ID 6) above */
                 /* if the spec means that one defines all types. */
                 /* Given the subtask, we expect a section for `deftype` which is Section ID 7 */
                if (section_id == 7) { /* Explicitly check for Component Type Section (deftype) */
                     LOG_VERBOSE("Attempting to load Component Type Section (ID 7) for deftypes.\n");
                     /* This is where the new load_type_section (for deftypes) should be called */
                     /* if (!load_type_section_for_deftypes(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; */
                     /* For now, as per previous stubs, we'll just skip and log */
                      os_printf("Found Component Type Section (ID 7) - deftype (stub).\n");
                       p += section_size; /* Skip content for now */
                } else {
                    os_printf("Skipping unknown component section ID: %u, size: %u\n",
                              section_id, section_size);
                    p += section_size;
                }
                break;
        }
        if (p != section_start + section_size) {
             set_error_buf_v(error_buf, error_buf_size,
                            "section size mismatch in section %u, expected %u but got %ld",
                            section_id, section_size, (long)(p - section_start));
            goto fail;
        }
    }

    return component;

fail:
    if (component) {
        wasm_component_unload(component);
    }
    return NULL;
}

void
wasm_component_unload(WASMComponent *component)
{
    if (!component) {
        return;
    }

    if (component->core_modules) {
        for (uint32 i = 0; i < component->core_module_count; i++) {
            if (component->core_modules[i].module_object) {
                wasm_loader_unload(component->core_modules[i].module_object);
            }
        }
        wasm_runtime_free(component->core_modules);
        component->core_modules = NULL; /* Avoid double free */
        component->core_module_count = 0;
    }

    /* TODO: Free other allocated resources within the component structure */
    /* Example for future sections:
    if (component->core_instances) {
        // ... free core_instances ...
        wasm_runtime_free(component->core_instances);
        component->core_instances = NULL;
        component->core_instance_count = 0;
    }
    if (component->aliases) {
        // ... free aliases ...
        wasm_runtime_free(component->aliases);
        component->aliases = NULL;
        component->alias_count = 0;
    }
    if (component->defined_types) {
        // ... free defined_types ...
        wasm_runtime_free(component->defined_types);
        component->defined_types = NULL;
        component->defined_type_count = 0;
    }
    */

    wasm_runtime_free(component);
}
