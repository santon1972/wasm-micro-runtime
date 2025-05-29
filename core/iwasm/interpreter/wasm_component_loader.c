/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_loader.h"
#include "wasm_component_canonical.h" /* <-- ADDED --> */
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
    const uint8 *section_content_end = p; 
    uint32 module_count, i;

    read_leb_uint32(p, *buf_end, module_count);
    LOG_VERBOSE("Component Core Module section with %u modules found.", module_count);

    section_content_end = p; 

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

        read_leb_uint32(p, *buf_end, module_len);
        core_module_entry->module_len = module_len;
        CHECK_BUF(p, *buf_end, module_len);
        core_module_entry->module_data = (uint8*)p;

        LOG_VERBOSE("Parsing core module %u, length %u.", i, module_len);

        if (!create_sections_from_core_module_data(
                core_module_entry->module_data, core_module_entry->module_len,
                &core_module_sections, error_buf, error_buf_size)) {
            goto fail;
        }

        core_module_entry->module_object = wasm_loader_load_from_sections(
            core_module_sections, error_buf, error_buf_size);

        if (core_module_sections) {
            destroy_sections(core_module_sections);
            core_module_sections = NULL;
        }

        if (!core_module_entry->module_object) {
            goto fail;
        }
        LOG_VERBOSE("Core module %u loaded successfully via sections.", i);
        p += module_len;
    }

    *p_buf = p;
    return true;

fail:
    return false;
}

static bool
parse_string(const uint8 **p_buf, const uint8 *buf_end,
             char **out_str,
             char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 len;

    read_leb_uint32(p, buf_end, len);
    CHECK_BUF(p, buf_end, len);

    *out_str = loader_malloc(len + 1, error_buf, error_buf_size);
    if (!*out_str) {
        return false; 
    }
    bh_memcpy_s(*out_str, len + 1, p, len);
    (*out_str)[len] = '\0';

    *p_buf = p + len;
    return true;
fail:
    if (*out_str) {
        wasm_runtime_free(*out_str);
        *out_str = NULL;
    }
    return false;
}

static bool
load_core_instance_section(const uint8 **p_buf, const uint8 *buf_end,
                           WASMComponent *component,
                           char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 core_instance_count, i, j;

    read_leb_uint32(p, buf_end, core_instance_count);
    LOG_VERBOSE("Component Core Instance section with %u instances.", core_instance_count);

    if (core_instance_count == 0) {
        *p_buf = p;
        return true;
    }

    if (component->core_instances) {
        set_error_buf(error_buf, error_buf_size, "duplicate core instance section");
        goto fail;
    }

    component->core_instances = loader_malloc(
        core_instance_count * sizeof(WASMComponentCoreInstance), error_buf, error_buf_size);
    if (!component->core_instances) {
        goto fail;
    }
    component->core_instance_count = core_instance_count;
    memset(component->core_instances, 0, core_instance_count * sizeof(WASMComponentCoreInstance));

    for (i = 0; i < core_instance_count; i++) {
        WASMComponentCoreInstance *current_instance = &component->core_instances[i];
        uint8 kind_byte;
        CHECK_BUF(p, buf_end, 1);
        kind_byte = *p++;
        current_instance->kind = (WASMCoreInstanceKind)kind_byte;

        if (current_instance->kind == CORE_INSTANCE_KIND_INSTANTIATE) {
            read_leb_uint32(p, buf_end, current_instance->u.instantiate.module_idx);
            read_leb_uint32(p, buf_end, current_instance->u.instantiate.arg_count);

            if (current_instance->u.instantiate.arg_count > 0) {
                current_instance->u.instantiate.args = loader_malloc(
                    current_instance->u.instantiate.arg_count * sizeof(WASMComponentCoreInstanceArg),
                    error_buf, error_buf_size);
                if (!current_instance->u.instantiate.args) {
                    goto fail;
                }
                memset(current_instance->u.instantiate.args, 0,
                       current_instance->u.instantiate.arg_count * sizeof(WASMComponentCoreInstanceArg));

                for (j = 0; j < current_instance->u.instantiate.arg_count; j++) {
                    WASMComponentCoreInstanceArg *arg = &current_instance->u.instantiate.args[j];
                    if (!parse_string(&p, buf_end, &arg->name, error_buf, error_buf_size)) {
                        goto fail;
                    }
                    read_leb_uint32(p, buf_end, arg->instance_idx);
                }
            }
        } else if (current_instance->kind == CORE_INSTANCE_KIND_INLINE_EXPORT) {
            read_leb_uint32(p, buf_end, current_instance->u.inline_export.export_count);
            if (current_instance->u.inline_export.export_count > 0) {
                 current_instance->u.inline_export.exports = loader_malloc(
                    current_instance->u.inline_export.export_count * sizeof(WASMComponentCoreInlineExport),
                    error_buf, error_buf_size);
                if (!current_instance->u.inline_export.exports) {
                    goto fail;
                }
                memset(current_instance->u.inline_export.exports, 0,
                       current_instance->u.inline_export.export_count * sizeof(WASMComponentCoreInlineExport));

                for (j = 0; j < current_instance->u.inline_export.export_count; j++) {
                    WASMComponentCoreInlineExport *export_item = &current_instance->u.inline_export.exports[j];
                    if (!parse_string(&p, buf_end, &export_item->name, error_buf, error_buf_size)) {
                        goto fail;
                    }
                    CHECK_BUF(p, buf_end, 1); 
                    export_item->kind = *p++;
                    read_leb_uint32(p, buf_end, export_item->sort_idx);
                }
            }
        } else {
            set_error_buf_v(error_buf, error_buf_size, "unknown core instance kind: %u", current_instance->kind);
            goto fail;
        }
    }

    *p_buf = p;
    return true;
fail:
    return false;
}

static bool
load_core_type_section(const uint8 **p_buf, const uint8 *buf_end,
                       WASMComponent *component,
                       char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 core_type_count, i;

    read_leb_uint32(p, buf_end, core_type_count);
    LOG_VERBOSE("Component Core Type section with %u types.", core_type_count);

    if (core_type_count == 0) {
        *p_buf = p;
        return true;
    }
    if (component->core_type_defs) {
        set_error_buf(error_buf, error_buf_size, "duplicate core type section");
        goto fail;
    }
    component->core_type_defs = loader_malloc(
        core_type_count * sizeof(WASMComponentCoreTypeDef), error_buf, error_buf_size);
    if (!component->core_type_defs) {
        goto fail;
    }
    component->core_type_def_count = core_type_count;
    memset(component->core_type_defs, 0, core_type_count * sizeof(WASMComponentCoreTypeDef));

    for (i = 0; i < core_type_count; i++) {
        WASMComponentCoreTypeDef *current_type = &component->core_type_defs[i];
        CHECK_BUF(p, buf_end, 1);
        current_type->kind = *p++;
        LOG_VERBOSE("Core Type Def %d, kind %02X (actual parsing might be deeper)", i, current_type->kind);
    }

    *p_buf = p;
    return true;
fail:
    return false;
}

static bool
load_component_section(const uint8 **p_buf, const uint8 *buf_end, /* Nested Component Section */
                       WASMComponent *component,
                       char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 nested_component_count, i;

    read_leb_uint32(p, buf_end, nested_component_count);
    LOG_VERBOSE("Component (nested) Component section with %u nested components.", nested_component_count);

    if (nested_component_count == 0) {
        *p_buf = p;
        return true;
    }
    if (component->nested_components) {
        set_error_buf(error_buf, error_buf_size, "duplicate nested component section");
        goto fail;
    }
    component->nested_components = loader_malloc(
        nested_component_count * sizeof(WASMComponentNestedComponent), error_buf, error_buf_size);
    if (!component->nested_components) {
        goto fail;
    }
    component->nested_component_count = nested_component_count;
    memset(component->nested_components, 0, nested_component_count * sizeof(WASMComponentNestedComponent));

    for (i = 0; i < nested_component_count; i++) {
        WASMComponentNestedComponent *current_nested = &component->nested_components[i];
        uint32 component_len;
        read_leb_uint32(p, buf_end, component_len);
        current_nested->component_len = component_len;
        CHECK_BUF(p, buf_end, component_len);
        current_nested->component_data = (uint8*)p; 
        current_nested->parsed_component = NULL; 
        p += component_len;
        LOG_VERBOSE("Nested component %u, length %u bytes.", i, component_len);
    }

    *p_buf = p;
    return true;
fail:
    return false;
}

static bool
load_instance_section(const uint8 **p_buf, const uint8 *buf_end, /* Component Instance Section */
                      WASMComponent *component,
                      char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 instance_count, i, j;

    read_leb_uint32(p, buf_end, instance_count);
    LOG_VERBOSE("Component Instance section with %u instances.", instance_count);

    if (instance_count == 0) {
        *p_buf = p;
        return true;
    }
    if (component->component_instances) { 
        set_error_buf(error_buf, error_buf_size, "duplicate instance section");
        goto fail;
    }
    component->component_instances = loader_malloc(
        instance_count * sizeof(WASMComponentInstance), error_buf, error_buf_size);
    if (!component->component_instances) {
        goto fail;
    }
    component->component_instance_count = instance_count;
    memset(component->component_instances, 0, instance_count * sizeof(WASMComponentInstance));

    for (i = 0; i < instance_count; i++) {
        WASMComponentInstance *current_inst = &component->component_instances[i];
        CHECK_BUF(p, buf_end, 1);
        current_inst->instance_kind = *p++; 
        read_leb_uint32(p, buf_end, current_inst->item_idx); 
        read_leb_uint32(p, buf_end, current_inst->arg_count);

        if (current_inst->arg_count > 0) {
            current_inst->args = loader_malloc(
                current_inst->arg_count * sizeof(WASMComponentInstanceArg), error_buf, error_buf_size);
            if (!current_inst->args) {
                goto fail;
            }
            memset(current_inst->args, 0, current_inst->arg_count * sizeof(WASMComponentInstanceArg));
            for (j = 0; j < current_inst->arg_count; j++) {
                WASMComponentInstanceArg *arg = &current_inst->args[j];
                if (!parse_string(&p, buf_end, &arg->name, error_buf, error_buf_size)) {
                    goto fail;
                }
                read_leb_uint32(p, buf_end, arg->item_idx);
                arg->kind = INSTANCE_ARG_KIND_ITEM_IDX; 
            }
        }
        LOG_VERBOSE("Instance %u: kind %s, item_idx %u, arg_count %u", i,
                    current_inst->instance_kind == 0 ? "core" : "component",
                    current_inst->item_idx, current_inst->arg_count);
    }

    *p_buf = p;
    return true;
fail:
    return false;
}

static bool
load_alias_section(const uint8 **p_buf, const uint8 *buf_end,
                   WASMComponent *component,
                   char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 alias_count, i;

    read_leb_uint32(p, buf_end, alias_count);
    LOG_VERBOSE("Component Alias section with %u aliases.", alias_count);

    if (alias_count == 0) {
        *p_buf = p;
        return true;
    }
    if (component->aliases) {
        set_error_buf(error_buf, error_buf_size, "duplicate alias section");
        goto fail;
    }
    component->aliases = loader_malloc(
        alias_count * sizeof(WASMComponentAlias), error_buf, error_buf_size);
    if (!component->aliases) {
        goto fail;
    }
    component->alias_count = alias_count;
    memset(component->aliases, 0, alias_count * sizeof(WASMComponentAlias));

    for (i = 0; i < alias_count; i++) {
        WASMComponentAlias *current_alias = &component->aliases[i];
        uint8 sort_byte;
        uint8 target_kind_byte;

        CHECK_BUF(p, buf_end, 1); 
        sort_byte = *p++;
        current_alias->sort = (WASMAliasSort)sort_byte;

        CHECK_BUF(p, buf_end, 1); 
        target_kind_byte = *p++;
        current_alias->target_kind = (WASMAliasTargetKind)target_kind_byte;

        current_alias->target_name = NULL; 

        switch (current_alias->target_kind) {
            case ALIAS_TARGET_CORE_EXPORT: 
                read_leb_uint32(p, buf_end, current_alias->target_idx); 
                if (!parse_string(&p, buf_end, &current_alias->target_name, error_buf, error_buf_size)) {
                    goto fail;
                }
                break;
            case ALIAS_TARGET_OUTER:
                 read_leb_uint32(p, buf_end, current_alias->target_outer_depth);
                 read_leb_uint32(p, buf_end, current_alias->target_idx);
                 break;
            case ALIAS_TARGET_CORE_MODULE: 
            case ALIAS_TARGET_TYPE:        
            case ALIAS_TARGET_COMPONENT:   
            case ALIAS_TARGET_INSTANCE:    
                read_leb_uint32(p, buf_end, current_alias->target_idx);
                break;
            default:
                set_error_buf_v(error_buf, error_buf_size, "unknown alias target kind: %u", current_alias->target_kind);
                goto fail;
        }
        LOG_VERBOSE("Alias %u: sort %u, target_kind %u, target_idx %u, target_name %s",
                    i, current_alias->sort, current_alias->target_kind, current_alias->target_idx,
                    current_alias->target_name ? current_alias->target_name : "N/A");
    }

    *p_buf = p;
    return true;
fail:
    return false;
}


/* Forward declaration of helper for parsing defined types */
static bool
parse_defined_type_entry(const uint8 **p_buf, const uint8 *buf_end,
                         WASMComponentDefinedType *defined_type,
                         char *error_buf, uint32 error_buf_size);

static bool
load_type_section(const uint8 **p_buf, const uint8 *buf_end, /* Type Section ID 6 */
                  WASMComponent *component,
                  char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 type_def_count, i;

    read_leb_uint32(p, buf_end, type_def_count);
    LOG_VERBOSE("Component Type section (ID 6) with %u type definitions.", type_def_count);

    if (type_def_count == 0) {
        *p_buf = p;
        return true;
    }
    if (component->type_definitions) {
        set_error_buf(error_buf, error_buf_size, "duplicate type section (ID 6)");
        goto fail;
    }
    component->type_definitions = loader_malloc(
        type_def_count * sizeof(WASMComponentDefinedType), error_buf, error_buf_size);
    if (!component->type_definitions) {
        goto fail;
    }
    component->type_definition_count = type_def_count;
    memset(component->type_definitions, 0, type_def_count * sizeof(WASMComponentDefinedType));

    for (i = 0; i < type_def_count; i++) {
        LOG_VERBOSE("Parsing type definition %u in Type Section (ID 6)", i);
        if (!parse_defined_type_entry(&p, buf_end, &component->type_definitions[i],
                                      error_buf, error_buf_size)) {
            goto fail;
        }
    }

    *p_buf = p;
    return true;
fail:
    return false;
}

static bool
load_component_defined_type_section(const uint8 **p_buf, const uint8 *buf_end, /* Component Type Section ID 7 */
                                   WASMComponent *component,
                                   char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 comp_type_def_count, i;

    read_leb_uint32(p, buf_end, comp_type_def_count);
    LOG_VERBOSE("Component Defined Type section (ID 7) with %u definitions.", comp_type_def_count);

    if (comp_type_def_count == 0) {
        *p_buf = p;
        return true;
    }
    /* This field in WASMComponent is component_type_definitions as per .h */
    if (component->component_type_definitions) {
        set_error_buf(error_buf, error_buf_size, "duplicate component defined type section (ID 7)");
        goto fail;
    }
    component->component_type_definitions = loader_malloc(
        comp_type_def_count * sizeof(WASMComponentDefinedType), error_buf, error_buf_size);
    if (!component->component_type_definitions) {
        goto fail;
    }
    component->component_type_definition_count = comp_type_def_count;
    memset(component->component_type_definitions, 0, comp_type_def_count * sizeof(WASMComponentDefinedType));

    for (i = 0; i < comp_type_def_count; i++) {
        LOG_VERBOSE("Parsing defined type %u in Component Defined Type Section (ID 7)", i);
        if (!parse_defined_type_entry(&p, buf_end, &component->component_type_definitions[i],
                                      error_buf, error_buf_size)) {
            goto fail;
        }
    }

    *p_buf = p;
    return true;
fail:
    return false;
}


/* ========================================================================== */
/* START: Type Parsing Logic                                                */
/* ========================================================================== */

/* Forward declaration for mutual recursion */
static bool parse_valtype(const uint8 **p_buf, const uint8 *buf_end,
                          WASMComponentValType *valtype,
                          char *error_buf, uint32 error_buf_size);

static void free_valtype_contents(WASMComponentValType *valtype);
static void free_func_type_contents(WASMComponentFuncType *func_type);
static void free_component_type_decl_contents(WASMComponentTypeDecl *type_decl);
static void free_component_type_contents(WASMComponentComponentType *comp_type);
static void free_instance_type_decl_contents(WASMComponentInstanceTypeDecl *inst_type_decl);
static void free_instance_type_contents(WASMComponentInstanceType *inst_type);
static void free_defined_type_contents(WASMComponentDefinedType *defined_type);


static WASMComponentPrimValType
map_binary_primitive_to_enum(uint8 binary_tag, bool *success) {
    *success = true;
    // Based on Binary.md `primitive-valtype`
    switch (binary_tag) {
        case 0x7B: return PRIM_VAL_BOOL;
        case 0x7A: return PRIM_VAL_S8;
        case 0x79: return PRIM_VAL_U8;
        case 0x78: return PRIM_VAL_S16;
        case 0x77: return PRIM_VAL_U16;
        case 0x76: return PRIM_VAL_S32;
        case 0x75: return PRIM_VAL_U32;
        case 0x74: return PRIM_VAL_S64;
        case 0x73: return PRIM_VAL_U64;
        case 0x72: return PRIM_VAL_F32;
        case 0x71: return PRIM_VAL_F64;
        case 0x70: return PRIM_VAL_CHAR;
        case 0x6F: return PRIM_VAL_STRING; // Spec uses 0x6F for string
        default:
            *success = false;
            return (WASMComponentPrimValType)0; /* Invalid */
    }
}

static bool
parse_valtype(const uint8 **p_buf, const uint8 *buf_end,
              WASMComponentValType *valtype,
              char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint8 tag;
    uint32 i, count;
    bool map_success;

    CHECK_BUF(p, buf_end, 1);
    tag = *p++; // Consume the valtype tag
    memset(valtype, 0, sizeof(WASMComponentValType));

    // Binary.md `valtype` encoding
    switch (tag) {
        // Primitive types 0x7B down to 0x6F
        case 0x7B: case 0x7A: case 0x79: case 0x78: case 0x77: case 0x76:
        case 0x75: case 0x74: case 0x73: case 0x72: case 0x71: case 0x70: case 0x6F:
            valtype->kind = VAL_TYPE_KIND_PRIMITIVE;
            valtype->u.primitive = map_binary_primitive_to_enum(tag, &map_success);
            if (!map_success) {
                set_error_buf_v(error_buf, error_buf_size, "unknown primitive valtype tag 0x%02X", tag);
                goto fail;
            }
            break;

        case 0x00: /* definedtypeidx (u32) - reference to a type definition */
            valtype->kind = VAL_TYPE_KIND_TYPE_IDX;
            read_leb_uint32(p, buf_end, valtype->u.type_idx);
            break;

        case 0x6E: /* record - (vec <record-field>) ; record-field := (label string) (type valtype) */
            valtype->kind = VAL_TYPE_KIND_RECORD;
            read_leb_uint32(p, buf_end, count);
            valtype->u.record.field_count = count;
            if (count > 0) {
                valtype->u.record.fields = loader_malloc(count * sizeof(WASMComponentLabelValType), error_buf, error_buf_size);
                if (!valtype->u.record.fields) goto fail;
                memset(valtype->u.record.fields, 0, count * sizeof(WASMComponentLabelValType));
                for (i = 0; i < count; i++) {
                    if (!parse_string(&p, buf_end, &valtype->u.record.fields[i].label, error_buf, error_buf_size)) goto fail;
                    valtype->u.record.fields[i].valtype = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
                    if (!valtype->u.record.fields[i].valtype) goto fail;
                    memset(valtype->u.record.fields[i].valtype, 0, sizeof(WASMComponentValType)); // For safe free on partial parse
                    if (!parse_valtype(&p, buf_end, valtype->u.record.fields[i].valtype, error_buf, error_buf_size)) goto fail;
                }
            } else {
                valtype->u.record.fields = NULL;
            }
            break;

        case 0x6D: /* variant - (vec <variant-case>) ; variant-case := (label string) (type (optional valtype)) (refines (optional u32)) */
            valtype->kind = VAL_TYPE_KIND_VARIANT;
            read_leb_uint32(p, buf_end, count);
            valtype->u.variant.case_count = count;
            if (count > 0) {
                valtype->u.variant.cases = loader_malloc(count * sizeof(WASMComponentCase), error_buf, error_buf_size);
                if (!valtype->u.variant.cases) goto fail;
                memset(valtype->u.variant.cases, 0, count * sizeof(WASMComponentCase));
                for (i = 0; i < count; i++) {
                    WASMComponentCase *current_case = &valtype->u.variant.cases[i];
                    if (!parse_string(&p, buf_end, &current_case->label, error_buf, error_buf_size)) goto fail;
                    
                    uint8 valtype_present;
                    CHECK_BUF(p, buf_end, 1);
                    valtype_present = *p++;
                    if (valtype_present == 0x01) {
                        current_case->valtype = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
                        if (!current_case->valtype) goto fail;
                        memset(current_case->valtype, 0, sizeof(WASMComponentValType)); // For safe free
                        if (!parse_valtype(&p, buf_end, current_case->valtype, error_buf, error_buf_size)) goto fail;
                    } else {
                        current_case->valtype = NULL;
                    }
                    
                    uint8 refines_present; 
                    CHECK_BUF(p, buf_end, 1);
                    refines_present = *p++;
                    if (refines_present == 0x01) {
                        read_leb_uint32(p, buf_end, current_case->default_case_idx); 
                    } else {
                        current_case->default_case_idx = (uint32)-1; 
                    }
                }
            } else {
                valtype->u.variant.cases = NULL;
            }
            break;

        case 0x6C: /* list - <valtype> */
            valtype->kind = VAL_TYPE_KIND_LIST;
            valtype->u.list.element_valtype = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
            if (!valtype->u.list.element_valtype) goto fail;
            memset(valtype->u.list.element_valtype, 0, sizeof(WASMComponentValType)); // For safe free
            if (!parse_valtype(&p, buf_end, valtype->u.list.element_valtype, error_buf, error_buf_size)) goto fail;
            break;

        case 0x6B: /* tuple - (vec <valtype>) */
            valtype->kind = VAL_TYPE_KIND_TUPLE;
            read_leb_uint32(p, buf_end, count);
            valtype->u.tuple.element_count = count;
            if (count > 0) {
                valtype->u.tuple.element_valtypes = loader_malloc(count * sizeof(WASMComponentValType), error_buf, error_buf_size);
                if (!valtype->u.tuple.element_valtypes) goto fail;
                memset(valtype->u.tuple.element_valtypes, 0, count * sizeof(WASMComponentValType)); 
                for (i = 0; i < count; i++) {
                    if (!parse_valtype(&p, buf_end, &valtype->u.tuple.element_valtypes[i], error_buf, error_buf_size)) goto fail;
                }
            } else {
                valtype->u.tuple.element_valtypes = NULL;
            }
            break;

        case 0x6A: /* flags - (vec <string>) */
            valtype->kind = VAL_TYPE_KIND_FLAGS;
            read_leb_uint32(p, buf_end, count);
            valtype->u.flags.label_count = count;
            if (count > 0) {
                valtype->u.flags.labels = loader_malloc(count * sizeof(char*), error_buf, error_buf_size);
                if (!valtype->u.flags.labels) goto fail;
                memset(valtype->u.flags.labels, 0, count * sizeof(char*));
                for (i = 0; i < count; i++) {
                    if (!parse_string(&p, buf_end, &valtype->u.flags.labels[i], error_buf, error_buf_size)) goto fail;
                }
            } else {
                valtype->u.flags.labels = NULL;
            }
            break;

        case 0x69: /* enum - (vec <string>) */
            valtype->kind = VAL_TYPE_KIND_ENUM;
            read_leb_uint32(p, buf_end, count);
            valtype->u.enum_type.label_count = count;
            if (count > 0) {
                valtype->u.enum_type.labels = loader_malloc(count * sizeof(char*), error_buf, error_buf_size);
                if (!valtype->u.enum_type.labels) goto fail;
                memset(valtype->u.enum_type.labels, 0, count * sizeof(char*));
                for (i = 0; i < count; i++) {
                    if (!parse_string(&p, buf_end, &valtype->u.enum_type.labels[i], error_buf, error_buf_size)) goto fail;
                }
            } else {
                valtype->u.enum_type.labels = NULL;
            }
            break;

        case 0x68: /* option - <valtype> */
            valtype->kind = VAL_TYPE_KIND_OPTION;
            valtype->u.option.valtype = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
            if (!valtype->u.option.valtype) goto fail;
            memset(valtype->u.option.valtype, 0, sizeof(WASMComponentValType)); // For safe free
            if (!parse_valtype(&p, buf_end, valtype->u.option.valtype, error_buf, error_buf_size)) goto fail;
            break;

        case 0x67: /* result - (ok (optional <valtype>)) (err (optional <valtype>)) */
            valtype->kind = VAL_TYPE_KIND_RESULT;
            valtype->u.result.ok_valtype = NULL; 
            valtype->u.result.err_valtype = NULL;

            uint8 ok_present;
            CHECK_BUF(p, buf_end, 1);
            ok_present = *p++;
            if (ok_present == 0x01) {
                valtype->u.result.ok_valtype = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
                if (!valtype->u.result.ok_valtype) goto fail;
                memset(valtype->u.result.ok_valtype, 0, sizeof(WASMComponentValType)); // For safe free
                if (!parse_valtype(&p, buf_end, valtype->u.result.ok_valtype, error_buf, error_buf_size)) goto fail;
            }
            
            uint8 err_present;
            CHECK_BUF(p, buf_end, 1);
            err_present = *p++;
            if (err_present == 0x01) {
                valtype->u.result.err_valtype = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
                if (!valtype->u.result.err_valtype) goto fail;
                memset(valtype->u.result.err_valtype, 0, sizeof(WASMComponentValType)); // For safe free
                if (!parse_valtype(&p, buf_end, valtype->u.result.err_valtype, error_buf, error_buf_size)) goto fail;
            }
            break;
        
        case 0x66: /* own - <typeidx to resource> */
            valtype->kind = VAL_TYPE_KIND_OWN_TYPE_IDX;
            read_leb_uint32(p, buf_end, valtype->u.type_idx);
            break;
        
        case 0x65: /* borrow - <typeidx to resource> */
            valtype->kind = VAL_TYPE_KIND_BORROW_TYPE_IDX;
            read_leb_uint32(p, buf_end, valtype->u.type_idx);
            break;

        default:
            set_error_buf_v(error_buf, error_buf_size, "unknown valtype tag 0x%02X", tag);
            goto fail;
    }

    LOG_VERBOSE("Parsed valtype: tag 0x%02X, kind %d", tag, valtype->kind);
    *p_buf = p;
    return true;
fail:
    free_valtype_contents(valtype); 
    memset(valtype, 0, sizeof(WASMComponentValType)); 
    return false;
}

static void
free_valtype_contents(WASMComponentValType *valtype)
{
    if (!valtype) return;

    uint32 i;
    switch (valtype->kind) {
        case VAL_TYPE_KIND_RECORD:
            if (valtype->u.record.fields) {
                for (i = 0; i < valtype->u.record.field_count; i++) {
                    if (valtype->u.record.fields[i].label) {
                        wasm_runtime_free(valtype->u.record.fields[i].label);
                    }
                    if (valtype->u.record.fields[i].valtype) {
                        free_valtype_contents(valtype->u.record.fields[i].valtype);
                        wasm_runtime_free(valtype->u.record.fields[i].valtype);
                    }
                }
                wasm_runtime_free(valtype->u.record.fields);
            }
            break;
        case VAL_TYPE_KIND_VARIANT:
            if (valtype->u.variant.cases) {
                for (i = 0; i < valtype->u.variant.case_count; i++) {
                    if (valtype->u.variant.cases[i].label) {
                        wasm_runtime_free(valtype->u.variant.cases[i].label);
                    }
                    if (valtype->u.variant.cases[i].valtype) {
                        free_valtype_contents(valtype->u.variant.cases[i].valtype);
                        wasm_runtime_free(valtype->u.variant.cases[i].valtype);
                    }
                }
                wasm_runtime_free(valtype->u.variant.cases);
            }
            break;
        case VAL_TYPE_KIND_LIST:
            if (valtype->u.list.element_valtype) {
                free_valtype_contents(valtype->u.list.element_valtype);
                wasm_runtime_free(valtype->u.list.element_valtype);
            }
            break;
        case VAL_TYPE_KIND_TUPLE:
            if (valtype->u.tuple.element_valtypes) {
                for (i = 0; i < valtype->u.tuple.element_count; i++) {
                    free_valtype_contents(&valtype->u.tuple.element_valtypes[i]);
                }
                wasm_runtime_free(valtype->u.tuple.element_valtypes);
            }
            break;
        case VAL_TYPE_KIND_FLAGS:
            if (valtype->u.flags.labels) {
                for (i = 0; i < valtype->u.flags.label_count; i++) {
                    if (valtype->u.flags.labels[i]) {
                        wasm_runtime_free(valtype->u.flags.labels[i]);
                    }
                }
                wasm_runtime_free(valtype->u.flags.labels);
            }
            break;
        case VAL_TYPE_KIND_ENUM:
            if (valtype->u.enum_type.labels) {
                for (i = 0; i < valtype->u.enum_type.label_count; i++) {
                    if (valtype->u.enum_type.labels[i]) {
                        wasm_runtime_free(valtype->u.enum_type.labels[i]);
                    }
                }
                wasm_runtime_free(valtype->u.enum_type.labels);
            }
            break;
        case VAL_TYPE_KIND_OPTION:
            if (valtype->u.option.valtype) {
                free_valtype_contents(valtype->u.option.valtype);
                wasm_runtime_free(valtype->u.option.valtype);
            }
            break;
        case VAL_TYPE_KIND_RESULT:
            if (valtype->u.result.ok_valtype) {
                free_valtype_contents(valtype->u.result.ok_valtype);
                wasm_runtime_free(valtype->u.result.ok_valtype);
            }
            if (valtype->u.result.err_valtype) {
                free_valtype_contents(valtype->u.result.err_valtype);
                wasm_runtime_free(valtype->u.result.err_valtype);
            }
            break;
        case VAL_TYPE_KIND_PRIMITIVE:
        case VAL_TYPE_KIND_TYPE_IDX:
        case VAL_TYPE_KIND_OWN_TYPE_IDX:
        case VAL_TYPE_KIND_BORROW_TYPE_IDX:
            /* No deep freeing needed for these */
            break;
        default:
            /* Should not happen for valid ValTypeKind */
            break;
    }
}


static void
free_func_type_contents(WASMComponentFuncType *func_type)
{
    if (!func_type) return;
    if (func_type->params) {
        for (uint32 i = 0; i < func_type->param_count; i++) {
            if (func_type->params[i].label) {
                wasm_runtime_free(func_type->params[i].label);
            }
            if (func_type->params[i].valtype) {
                free_valtype_contents(func_type->params[i].valtype);
                wasm_runtime_free(func_type->params[i].valtype);
            }
        }
        wasm_runtime_free(func_type->params);
    }
    if (func_type->result) {
        free_valtype_contents(func_type->result);
        wasm_runtime_free(func_type->result);
    }
}

static bool
parse_func_type(const uint8 **p_buf, const uint8 *buf_end,
                WASMComponentFuncType *func_type,
                char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 i, param_count;
    memset(func_type, 0, sizeof(WASMComponentFuncType));

    // Params: (vec ((name string) (type valtype)))
    read_leb_uint32(p, buf_end, param_count);
    func_type->param_count = param_count;
    if (param_count > 0) {
        func_type->params = loader_malloc(param_count * sizeof(WASMComponentLabelValType), error_buf, error_buf_size);
        if (!func_type->params) goto fail;
        memset(func_type->params, 0, param_count * sizeof(WASMComponentLabelValType));
        for (i = 0; i < param_count; i++) {
            if (!parse_string(&p, buf_end, &func_type->params[i].label, error_buf, error_buf_size)) goto fail;
            func_type->params[i].valtype = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
            if (!func_type->params[i].valtype) goto fail;
            memset(func_type->params[i].valtype, 0, sizeof(WASMComponentValType));
            if (!parse_valtype(&p, buf_end, func_type->params[i].valtype, error_buf, error_buf_size)) goto fail;
        }
    } else {
        func_type->params = NULL;
    }

    // Result: (0x00 -> void | 0x01 <valtype>)
    uint8 result_tag;
    CHECK_BUF(p, buf_end, 1);
    result_tag = *p++;
    if (result_tag == 0x01) { // One valtype for result
        func_type->result = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
        if (!func_type->result) goto fail;
        memset(func_type->result, 0, sizeof(WASMComponentValType));
        if (!parse_valtype(&p, buf_end, func_type->result, error_buf, error_buf_size)) goto fail;
    } else if (result_tag == 0x00) { // Void result
        func_type->result = NULL;
    } else {
        set_error_buf_v(error_buf, error_buf_size, "invalid result tag for func type: 0x%02X", result_tag);
        goto fail;
    }
    
    LOG_VERBOSE("Parsed func_type: params %u, result %s", func_type->param_count, func_type->result ? "present" : "void");
    *p_buf = p;
    return true;
fail:
    free_func_type_contents(func_type); 
    return false;
}

static void
free_extern_desc_contents(WASMComponentExternDesc *desc) {
    if (!desc) return;
    if (desc->kind == EXTERN_DESC_KIND_VALUE && desc->u.value_type) {
        free_valtype_contents(desc->u.value_type);
        wasm_runtime_free(desc->u.value_type);
        desc->u.value_type = NULL;
    }
    // Other fields in ExternDesc (type_idx, etc.) don't need deep free.
}

static void
free_component_type_decl_contents(WASMComponentTypeDecl *type_decl)
{
    if (!type_decl) return;
    switch (type_decl->kind) {
        case COMPONENT_TYPE_DECL_KIND_IMPORT:
            if (type_decl->u.import_decl.name) {
                wasm_runtime_free(type_decl->u.import_decl.name);
            }
            free_extern_desc_contents(&type_decl->u.import_decl.desc);
            break;
        case COMPONENT_TYPE_DECL_KIND_EXPORT:
            if (type_decl->u.export_decl.name) {
                wasm_runtime_free(type_decl->u.export_decl.name);
            }
            free_extern_desc_contents(&type_decl->u.export_decl.desc);
            break;
        case COMPONENT_TYPE_DECL_KIND_ALIAS:
            if (type_decl->u.alias_decl.new_name) { 
                wasm_runtime_free(type_decl->u.alias_decl.new_name);
            }
            if (type_decl->u.alias_decl.target_name) { 
                wasm_runtime_free(type_decl->u.alias_decl.target_name);
            }
            break;
        case COMPONENT_TYPE_DECL_KIND_TYPE:
        case COMPONENT_TYPE_DECL_KIND_CORE_TYPE:
            free_defined_type_contents(&type_decl->u.type_def);
            break;
    }
}

static void
free_instance_type_decl_contents(WASMComponentInstanceTypeDecl *inst_type_decl)
{
    if (!inst_type_decl) return;
    switch (inst_type_decl->kind) {
        case INSTANCE_TYPE_DECL_KIND_EXPORT:
            if (inst_type_decl->u.export_decl.name) {
                wasm_runtime_free(inst_type_decl->u.export_decl.name);
            }
            free_extern_desc_contents(&inst_type_decl->u.export_decl.desc);
            break;
        case INSTANCE_TYPE_DECL_KIND_ALIAS:
             if (inst_type_decl->u.alias_decl.new_name) {
                wasm_runtime_free(inst_type_decl->u.alias_decl.new_name);
            }
            if (inst_type_decl->u.alias_decl.target_name) {
                wasm_runtime_free(inst_type_decl->u.alias_decl.target_name);
            }
            break;
        case INSTANCE_TYPE_DECL_KIND_TYPE:
        case INSTANCE_TYPE_DECL_KIND_CORE_TYPE:
            free_defined_type_contents(&inst_type_decl->u.type_def);
            break;
    }
}


static void
free_component_type_contents(WASMComponentComponentType *comp_type)
{
    if (!comp_type || !comp_type->decls) return;
    for (uint32 i = 0; i < comp_type->decl_count; ++i) {
        free_component_type_decl_contents(&comp_type->decls[i]);
    }
    wasm_runtime_free(comp_type->decls);
    comp_type->decls = NULL;
    comp_type->decl_count = 0;
}

static void
free_instance_type_contents(WASMComponentInstanceType *inst_type)
{
    if (!inst_type || !inst_type->decls) return;
    for (uint32 i = 0; i < inst_type->decl_count; ++i) {
        free_instance_type_decl_contents(&inst_type->decls[i]);
    }
    wasm_runtime_free(inst_type->decls);
    inst_type->decls = NULL;
    inst_type->decl_count = 0;
}


static void
free_defined_type_contents(WASMComponentDefinedType *defined_type)
{
    if (!defined_type) return;

    switch (defined_type->kind) {
        case DEF_TYPE_KIND_VALTYPE:
            free_valtype_contents(&defined_type->u.valtype);
            break;
        case DEF_TYPE_KIND_FUNC:
            free_func_type_contents(&defined_type->u.func_type);
            break;
        case DEF_TYPE_KIND_COMPONENT:
            if (defined_type->u.comp_type) {
                free_component_type_contents(defined_type->u.comp_type);
                wasm_runtime_free(defined_type->u.comp_type);
            }
            break;
        case DEF_TYPE_KIND_INSTANCE:
            if (defined_type->u.inst_type) {
                free_instance_type_contents(defined_type->u.inst_type);
                wasm_runtime_free(defined_type->u.inst_type);
            }
            break;
        case DEF_TYPE_KIND_RESOURCE:
            /* WASMComponentResourceType is simple, no deep free needed for u.res_type itself */
            break;
        case DEF_TYPE_KIND_CORE_MODULE:
            /* TODO: Define and free WASMComponentCoreModuleType if it's parsed elaborately */
            break;
        default:
            break;
    }
}


static bool
parse_component_type_decl(const uint8 **p_buf, const uint8 *buf_end,
                          WASMComponentTypeDecl *decl,
                          char *error_buf, uint32 error_buf_size)
{
    const uint8* p = *p_buf;
    uint8 kind_byte;
    CHECK_BUF(p, buf_end, 1);
    kind_byte = *p++;
    decl->kind = (WASMComponentTypeDeclKind)kind_byte;
    memset(&decl->u, 0, sizeof(decl->u));

    switch(decl->kind) {
        case COMPONENT_TYPE_DECL_KIND_IMPORT:
            // Binary: (name string) (desc extern_desc)
            if (!parse_string(&p, buf_end, &decl->u.import_decl.name, error_buf, error_buf_size)) goto fail_decl;
            if (!parse_extern_desc(&p, buf_end, &decl->u.import_decl.desc, error_buf, error_buf_size)) goto fail_decl;
            break;
        case COMPONENT_TYPE_DECL_KIND_EXPORT:
            // Binary: (name string) (desc extern_desc)
            if (!parse_string(&p, buf_end, &decl->u.export_decl.name, error_buf, error_buf_size)) goto fail_decl;
            if (!parse_extern_desc(&p, buf_end, &decl->u.export_decl.desc, error_buf, error_buf_size)) goto fail_decl;
            break;
        case COMPONENT_TYPE_DECL_KIND_ALIAS:
            // Binary: (name string) (sort <sort>) (target <aliastarget>)
            // The `new_name` field in WASMComponentTypeAliasDecl is for the alias name.
            if (!parse_string(&p, buf_end, &decl->u.alias_decl.new_name, error_buf, error_buf_size)) goto fail_decl;
            CHECK_BUF(p, buf_end, 1); // sort
            decl->u.alias_decl.sort = (WASMAliasSort)*p++;
            CHECK_BUF(p, buf_end, 1); // target_kind
            decl->u.alias_decl.target_kind = (WASMAliasTargetKind)*p++;
            
            // Parse target specific data based on target_kind
            switch (decl->u.alias_decl.target_kind) {
                case ALIAS_TARGET_CORE_EXPORT: // (core export <instance_idx> <name>)
                    read_leb_uint32(p, buf_end, decl->u.alias_decl.target_idx); // core instance index
                    if (!parse_string(&p, buf_end, &decl->u.alias_decl.target_name, error_buf, error_buf_size)) goto fail_decl;
                    break;
                case ALIAS_TARGET_OUTER: // (outer <depth> <idx>)
                    read_leb_uint32(p, buf_end, decl->u.alias_decl.target_outer_depth);
                    read_leb_uint32(p, buf_end, decl->u.alias_decl.target_idx);
                    break;
                case ALIAS_TARGET_CORE_MODULE:
                case ALIAS_TARGET_TYPE:
                case ALIAS_TARGET_COMPONENT:
                case ALIAS_TARGET_INSTANCE:
                    read_leb_uint32(p, buf_end, decl->u.alias_decl.target_idx);
                    break;
                default:
                    set_error_buf_v(error_buf, error_buf_size, "unknown alias target kind in component type decl: %u", decl->u.alias_decl.target_kind);
                    goto fail_decl;
            }
            break;
        case COMPONENT_TYPE_DECL_KIND_TYPE: 
        case COMPONENT_TYPE_DECL_KIND_CORE_TYPE: 
            if (!parse_defined_type_entry(&p, buf_end, &decl->u.type_def, error_buf, error_buf_size)) goto fail_decl;
            break;
        default:
            set_error_buf_v(error_buf, error_buf_size, "unknown component_type_decl kind: %u", decl->kind);
            goto fail_decl;
    }
    *p_buf = p;
    return true;
fail_decl:
    free_component_type_decl_contents(decl);
    return false;
}


static bool
parse_component_type(const uint8 **p_buf, const uint8 *buf_end,
                     WASMComponentComponentType *comp_type,
                     char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 i;
    memset(comp_type, 0, sizeof(WASMComponentComponentType));
    /* componenttype := (decl (vec component-type-decl)) */
    read_leb_uint32(p, buf_end, comp_type->decl_count);
    if (comp_type->decl_count > 0) {
        comp_type->decls = loader_malloc(comp_type->decl_count * sizeof(WASMComponentTypeDecl),
                                         error_buf, error_buf_size);
        if (!comp_type->decls) goto fail;
        memset(comp_type->decls, 0, comp_type->decl_count * sizeof(WASMComponentTypeDecl));
        for (i = 0; i < comp_type->decl_count; ++i) {
            if (!parse_component_type_decl(&p, buf_end, &comp_type->decls[i], error_buf, error_buf_size)) {
                goto fail;
            }
        }
    } else {
        comp_type->decls = NULL;
    }
    *p_buf = p;
    LOG_VERBOSE("Parsed component type with %d decls", comp_type->decl_count);
    return true;
fail:
    free_component_type_contents(comp_type); // Cleans up partially parsed decls
    return false;
}


static bool
parse_instance_type_decl(const uint8 **p_buf, const uint8 *buf_end,
                          WASMComponentInstanceTypeDecl *decl,
                          char *error_buf, uint32 error_buf_size)
{
    const uint8* p = *p_buf;
    uint8 kind_byte;
    CHECK_BUF(p, buf_end, 1);
    kind_byte = *p++;
    decl->kind = (WASMComponentInstanceTypeDeclKind)kind_byte;
    memset(&decl->u, 0, sizeof(decl->u));

    switch(decl->kind) {
        case INSTANCE_TYPE_DECL_KIND_EXPORT:
            if (!parse_string(&p, buf_end, &decl->u.export_decl.name, error_buf, error_buf_size)) goto fail_decl;
            if (!parse_extern_desc(&p, buf_end, &decl->u.export_decl.desc, error_buf, error_buf_size)) goto fail_decl;
            break;
        case INSTANCE_TYPE_DECL_KIND_ALIAS:
            // Binary: (name string) (sort <sort>) (target <aliastarget>)
            if (!parse_string(&p, buf_end, &decl->u.alias_decl.new_name, error_buf, error_buf_size)) goto fail_decl;
            CHECK_BUF(p, buf_end, 1); // sort
            decl->u.alias_decl.sort = (WASMAliasSort)*p++;
            CHECK_BUF(p, buf_end, 1); // target_kind
            decl->u.alias_decl.target_kind = (WASMAliasTargetKind)*p++;
            
            switch (decl->u.alias_decl.target_kind) {
                case ALIAS_TARGET_CORE_EXPORT: 
                    read_leb_uint32(p, buf_end, decl->u.alias_decl.target_idx); 
                    if (!parse_string(&p, buf_end, &decl->u.alias_decl.target_name, error_buf, error_buf_size)) goto fail_decl;
                    break;
                case ALIAS_TARGET_OUTER: 
                    read_leb_uint32(p, buf_end, decl->u.alias_decl.target_outer_depth);
                    read_leb_uint32(p, buf_end, decl->u.alias_decl.target_idx);
                    break;
                case ALIAS_TARGET_CORE_MODULE:
                case ALIAS_TARGET_TYPE:
                case ALIAS_TARGET_COMPONENT:
                case ALIAS_TARGET_INSTANCE:
                    read_leb_uint32(p, buf_end, decl->u.alias_decl.target_idx);
                    break;
                default:
                    set_error_buf_v(error_buf, error_buf_size, "unknown alias target kind in instance type decl: %u", decl->u.alias_decl.target_kind);
                    goto fail_decl;
            }
            break;
        case INSTANCE_TYPE_DECL_KIND_TYPE:
        case INSTANCE_TYPE_DECL_KIND_CORE_TYPE:
            if (!parse_defined_type_entry(&p, buf_end, &decl->u.type_def, error_buf, error_buf_size)) goto fail_decl;
            break;
        default:
            set_error_buf_v(error_buf, error_buf_size, "unknown instance_type_decl kind: %u", decl->kind);
            goto fail_decl;
    }
    *p_buf = p;
    return true;
fail_decl:
    free_instance_type_decl_contents(decl);
    return false;
}


static bool
parse_instance_type(const uint8 **p_buf, const uint8 *buf_end,
                    WASMComponentInstanceType *inst_type,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 i;
    memset(inst_type, 0, sizeof(WASMComponentInstanceType));
    /* instancetype := (decl (vec instance-type-decl)) */
    read_leb_uint32(p, buf_end, inst_type->decl_count);
     if (inst_type->decl_count > 0) {
        inst_type->decls = loader_malloc(inst_type->decl_count * sizeof(WASMComponentInstanceTypeDecl),
                                         error_buf, error_buf_size);
        if (!inst_type->decls) goto fail;
        memset(inst_type->decls, 0, inst_type->decl_count * sizeof(WASMComponentInstanceTypeDecl));
        for (i = 0; i < inst_type->decl_count; ++i) {
            if (!parse_instance_type_decl(&p, buf_end, &inst_type->decls[i], error_buf, error_buf_size)) {
                goto fail;
            }
        }
    } else {
        inst_type->decls = NULL;
    }
    *p_buf = p;
    LOG_VERBOSE("Parsed instance type with %d decls", inst_type->decl_count);
    return true;
fail:
    free_instance_type_contents(inst_type);
    return false;
}

static bool
parse_resource_type(const uint8 **p_buf, const uint8 *buf_end,
                    WASMComponentResourceType *res_type,
                    char *error_buf, uint32 error_buf_size)
{
    /* Spec: (resource <rep> <dtor-optional>) 
       <rep> is a valtype. <dtor-optional> is (0x00 | 0x01 <core func idx>)
       The struct WASMComponentResourceType has `uint32 rep;` and `uint32 dtor_func_idx;`
       This implies `rep` in the struct is a simplified representation (e.g. a primitive valtype tag).
       The original .h comment: "Expected to be 0x7F (i32) for now".
       Binary.md for `resourcetype`: `0x43 <valtype> <dtor_opt>` where `dtor_opt` is a `u32` for func idx, or a special value.
       The `0x43` is the prefix for `resourcetype` in `type-def`.
       The actual content of `resourcetype` is `(rep valtype) (dtor (optional corefuncidx))`.
       Let's assume `rep` in the struct is meant to store the primitive tag of the valtype if it is primitive.
    */
    const uint8 *p = *p_buf;
    WASMComponentValType rep_valtype; // Temporary to parse the valtype for rep
    uint8 dtor_present;
    // bool map_success; // Not needed if we store the original tag

    memset(res_type, 0, sizeof(WASMComponentResourceType));
    memset(&rep_valtype, 0, sizeof(WASMComponentValType));

    // The `resourcetype` definition (after 0x43 prefix) starts with a `valtype` for representation.
    if (!parse_valtype(&p, buf_end, &rep_valtype, error_buf, error_buf_size)) {
        goto fail; // Error already set
    }

    // Store the original binary tag of the primitive type if rep is primitive.
    // This requires parse_valtype to have been called when p pointed to the tag,
    // and the tag itself should be stored or derived.
    // This is simplified: assume the loader expects the PRIM_VAL_... enum value if it's primitive.
    // The struct field `rep` is uint32. The comment "Expected to be 0x7F (i32)" means the binary tag.
    // This is tricky. For now, if it's a primitive, we'll store its enum value.
    // A more robust way would be for parse_valtype to return the original tag if needed,
    // or the struct should store WASMComponentValType* rep.
    if (rep_valtype.kind == VAL_TYPE_KIND_PRIMITIVE) {
        // This assumes res_type->rep is meant to hold the WASMComponentPrimValType enum.
        // This is a mismatch with the comment "0x7F (i32)".
        // Let's assume the struct wants the enum.
        res_type->rep = (uint32)rep_valtype.u.primitive;
    } else {
        set_error_buf(error_buf, error_buf_size, "resource representation must be a primitive type (current loader limitation)");
        free_valtype_contents(&rep_valtype);
        goto fail;
    }
    free_valtype_contents(&rep_valtype); // Free the temporary valtype as its info is copied or no longer needed

    CHECK_BUF(p, buf_end, 1);
    dtor_present = *p++;
    if (dtor_present == 0x01) { /* dtor is present */
        read_leb_uint32(p, buf_end, res_type->dtor_func_idx);
    } else {
        res_type->dtor_func_idx = (uint32)-1; /* Sentinel for not present */
    }
    LOG_VERBOSE("Parsed resource type: rep_primitive_enum %u, dtor_idx %u", res_type->rep, res_type->dtor_func_idx);
    *p_buf = p;
    return true;
fail:
    return false;
}


/* Main parser for an entry in Type Section (ID 6) or Component Defined Type Section (ID 7) */
static bool
parse_defined_type_entry(const uint8 **p_buf, const uint8 *buf_end,
                         WASMComponentDefinedType *defined_type,
                         char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint8 kind_byte; 
    CHECK_BUF(p, buf_end, 1);
    kind_byte = *p; /* Peek first */
    memset(defined_type, 0, sizeof(WASMComponentDefinedType));


    LOG_VERBOSE("Parsing defined_type_entry, kind_byte: 0x%02x", kind_byte);

    /* Based on Binary.md `type-def` encoding:
       A `type-def` can be a `valtype` directly, or a prefixed type like `functype`.
       Prefixes:
       0x40: functype
       0x41: componenttype
       0x42: instancetype
       0x43: resourcetype
       0x44: core:module type (TODO)
       If it's not one of these, it's a `valtype` definition.
    */

    switch (kind_byte) {
        case 0x40: /* functype */
            p++; /* Consume 0x40 kind_byte */
            defined_type->kind = DEF_TYPE_KIND_FUNC;
            if (!parse_func_type(&p, buf_end, &defined_type->u.func_type, error_buf, error_buf_size)) {
                goto fail;
            }
            break;
        case 0x41: /* componenttype */
            p++; /* Consume 0x41 */
            defined_type->kind = DEF_TYPE_KIND_COMPONENT;
            defined_type->u.comp_type = loader_malloc(sizeof(WASMComponentComponentType), error_buf, error_buf_size);
            if (!defined_type->u.comp_type) goto fail;
            //memset(defined_type->u.comp_type, 0, sizeof(WASMComponentComponentType)); // Done by parse_component_type
            if (!parse_component_type(&p, buf_end, defined_type->u.comp_type, error_buf, error_buf_size)) {
                goto fail;
            }
            break;
        case 0x42: /* instancetype */
            p++; /* Consume 0x42 */
            defined_type->kind = DEF_TYPE_KIND_INSTANCE;
            defined_type->u.inst_type = loader_malloc(sizeof(WASMComponentInstanceType), error_buf, error_buf_size);
            if (!defined_type->u.inst_type) goto fail;
            //memset(defined_type->u.inst_type, 0, sizeof(WASMComponentInstanceType)); // Done by parse_instance_type
            if (!parse_instance_type(&p, buf_end, defined_type->u.inst_type, error_buf, error_buf_size)) {
                goto fail;
            }
            break;
        case 0x43: /* resourcetype */
            p++; /* Consume 0x43 */
            defined_type->kind = DEF_TYPE_KIND_RESOURCE;
            if (!parse_resource_type(&p, buf_end, &defined_type->u.res_type, error_buf, error_buf_size)) {
                goto fail;
            }
            break;
        /* TODO: Add DEF_TYPE_KIND_CORE_MODULE (0x44) if needed */
        default:
            /* If not a prefixed type, it must be a valtype definition */
            /* A valtype definition itself is just a valtype, it does not have a 0x00 prefix byte in this context.
               The 0x00 valtype tag is for `definedtypeidx` when a valtype refers to another defined type.
               So, here, `p` is not advanced before calling `parse_valtype`.
            */
            defined_type->kind = DEF_TYPE_KIND_VALTYPE;
            if (!parse_valtype(&p, buf_end, &defined_type->u.valtype, error_buf, error_buf_size)) {
                goto fail;
            }
            break;
    }

    *p_buf = p;
    return true;
fail:
    // Cleanup logic for defined_type's union members if partially allocated
    // Note: The individual parsers (parse_func_type, parse_valtype) are expected
    // to clean up their own allocations on failure.
    // For comp_type and inst_type, if they were allocated but their parsing failed,
    // they should be freed here.
    if (defined_type->kind == DEF_TYPE_KIND_COMPONENT && defined_type->u.comp_type) {
        // If parse_component_type failed after allocating comp_type, it should have freed its own contents.
        // We just free the top-level comp_type pointer.
        wasm_runtime_free(defined_type->u.comp_type);
    } else if (defined_type->kind == DEF_TYPE_KIND_INSTANCE && defined_type->u.inst_type) {
        wasm_runtime_free(defined_type->u.inst_type);
    }
    // Ensure defined_type is zeroed out if it was partially set.
    memset(defined_type, 0, sizeof(WASMComponentDefinedType));
    return false;
}

/* ========================================================================== */
/* END: Type Parsing Logic                                                  */
/* ========================================================================== */


static bool
load_canonical_section(const uint8 **p_buf, const uint8 *buf_end,
                       WASMComponent *component,
                       char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 canonical_count, i, j;

    read_leb_uint32(p, buf_end, canonical_count);
    LOG_VERBOSE("Component Canonical section with %u functions.", canonical_count);

    if (canonical_count == 0) {
        *p_buf = p;
        return true;
    }
    if (component->canonicals) {
        set_error_buf(error_buf, error_buf_size, "duplicate canonical section");
        goto fail;
    }
    component->canonicals = loader_malloc(
        canonical_count * sizeof(WASMComponentCanonical), error_buf, error_buf_size);
    if (!component->canonicals) {
        goto fail;
    }
    component->canonical_count = canonical_count;
    memset(component->canonicals, 0, canonical_count * sizeof(WASMComponentCanonical));

    for (i = 0; i < canonical_count; i++) {
        WASMComponentCanonical *current_canon = &component->canonicals[i];
        uint8 kind_byte;
        CHECK_BUF(p, buf_end, 1); /* func_kind */
        kind_byte = *p++;
        current_canon->func_kind = (WASMCanonicalFuncKind)kind_byte;

        read_leb_uint32(p, buf_end, current_canon->core_func_idx);
        read_leb_uint32(p, buf_end, current_canon->component_func_type_idx);
        read_leb_uint32(p, buf_end, current_canon->option_count);

        if (current_canon->option_count > 0) {
            current_canon->options = loader_malloc(
                current_canon->option_count * sizeof(WASMComponentCanonicalOption), error_buf, error_buf_size);
            if (!current_canon->options) {
                goto fail;
            }
            memset(current_canon->options, 0, current_canon->option_count * sizeof(WASMComponentCanonicalOption));
            for (j = 0; j < current_canon->option_count; j++) {
                WASMComponentCanonicalOption *opt = &current_canon->options[j];
                uint8 opt_kind_byte;
                CHECK_BUF(p, buf_end, 1);
                opt_kind_byte = *p++;
                opt->kind = (WASMComponentCanonicalOptionKind)opt_kind_byte;
                switch (opt->kind) {
                    case CANONICAL_OPTION_STRING_ENCODING_UTF8:
                    case CANONICAL_OPTION_STRING_ENCODING_UTF16:
                    case CANONICAL_OPTION_STRING_ENCODING_COMPACT_UTF16:
                        /* No value to read for these */
                        break;
                    case CANONICAL_OPTION_MEMORY_IDX:
                    case CANONICAL_OPTION_REALLOC_FUNC_IDX:
                    case CANONICAL_OPTION_POST_RETURN_FUNC_IDX:
                        read_leb_uint32(p, buf_end, opt->value);
                        break;
                    default:
                        set_error_buf_v(error_buf, error_buf_size, "unknown canonical option kind: %u", opt->kind);
                        goto fail;
                }
            }
        }
        LOG_VERBOSE("Canonical func %u: kind %s, core_func %u, comp_func_type %u, options %u",
                    i, current_canon->func_kind == CANONICAL_FUNC_KIND_LIFT ? "lift" : "lower",
                    current_canon->core_func_idx, current_canon->component_func_type_idx, current_canon->option_count);
    }
    *p_buf = p;
    return true;
fail:
    return false;
}


static bool
load_start_section(const uint8 **p_buf, const uint8 *buf_end,
                   WASMComponent *component,
                   char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 start_count; /* Spec implies one start function, but use count for future proofing / vector format */

    read_leb_uint32(p, buf_end, start_count); /* This is likely always 0 or 1 based on current spec */
    LOG_VERBOSE("Component Start section with %u start functions.", start_count);

    if (start_count == 0) {
        component->start_count = 0;
        component->starts = NULL; /* Ensure it's NULL if count is 0 */
        *p_buf = p;
        return true;
    }
    if (start_count > 1) {
        /* Current spec: (section 8 (start <startfunc>)) - implies at most one. */
        set_error_buf(error_buf, error_buf_size, "multiple start functions not supported by current spec");
        goto fail;
    }

    if (component->starts) { /* Should be NULL at this point if start_count was 0 before */
        set_error_buf(error_buf, error_buf_size, "duplicate start section or invalid state");
        goto fail;
    }

    component->starts = loader_malloc(sizeof(WASMComponentStart), error_buf, error_buf_size); /* Only one for now */
    if (!component->starts) {
        goto fail;
    }
    component->start_count = 1;
    memset(component->starts, 0, sizeof(WASMComponentStart));

    WASMComponentStart *start_func = component->starts; /* Only one */
    read_leb_uint32(p, buf_end, start_func->func_idx);
    read_leb_uint32(p, buf_end, start_func->arg_count);

    if (start_func->arg_count > 0) {
        start_func->arg_value_indices = loader_malloc(
            start_func->arg_count * sizeof(uint32), error_buf, error_buf_size);
        if (!start_func->arg_value_indices) {
            goto fail;
        }
        for (uint32 i = 0; i < start_func->arg_count; i++) {
            read_leb_uint32(p, buf_end, start_func->arg_value_indices[i]);
        }
    } else {
        start_func->arg_value_indices = NULL;
    }
    LOG_VERBOSE("Start function: func_idx %u, arg_count %u", start_func->func_idx, start_func->arg_count);

    *p_buf = p;
    return true;
fail:
    if (component && component->starts) { /* Check component as well, as it might be NULL on other failures */
        if (component->starts->arg_value_indices) {
            wasm_runtime_free(component->starts->arg_value_indices);
        }
        wasm_runtime_free(component->starts);
        component->starts = NULL;
        component->start_count = 0;
    }
    return false;
}

/* Helper to parse ExternDesc */
static bool
parse_extern_desc(const uint8 **p_buf, const uint8 *buf_end,
                  WASMComponentExternDesc *desc,
                  char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint8 kind_byte;
    CHECK_BUF(p, buf_end, 1);
    kind_byte = *p++;
    desc->kind = (WASMComponentExternDescKind)kind_byte;
    memset(&desc->u, 0, sizeof(desc->u)); // Initialize union

    switch (desc->kind) {
        case EXTERN_DESC_KIND_MODULE:
            read_leb_uint32(p, buf_end, desc->u.core_module_type_idx);
            break;
        case EXTERN_DESC_KIND_FUNC:
            read_leb_uint32(p, buf_end, desc->u.func_type_idx);
            break;
        case EXTERN_DESC_KIND_VALUE:
            /* Value requires parsing a valtype */
            desc->u.value_type = loader_malloc(sizeof(WASMComponentValType), error_buf, error_buf_size);
            if (!desc->u.value_type) goto fail;
            memset(desc->u.value_type, 0, sizeof(WASMComponentValType));
            if (!parse_valtype(&p, buf_end, desc->u.value_type, error_buf, error_buf_size)) {
                wasm_runtime_free(desc->u.value_type); desc->u.value_type = NULL;
                goto fail;
            }
            break;
        case EXTERN_DESC_KIND_TYPE:
            CHECK_BUF(p, buf_end, 1); /* bound kind byte */
            desc->u.type_bound.kind = (WASMComponentTypeBoundKind)*p++;
            read_leb_uint32(p, buf_end, desc->u.type_bound.type_idx);
            break;
        case EXTERN_DESC_KIND_INSTANCE:
            read_leb_uint32(p, buf_end, desc->u.instance_type_idx);
            break;
        case EXTERN_DESC_KIND_COMPONENT:
            read_leb_uint32(p, buf_end, desc->u.component_type_idx);
            break;
        default:
            set_error_buf_v(error_buf, error_buf_size, "unknown extern_desc kind: %u", desc->kind);
            goto fail;
    }
    *p_buf = p;
    return true;
fail:
    return false;
}


static bool
load_import_section(const uint8 **p_buf, const uint8 *buf_end,
                    WASMComponent *component,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 import_count, i;

    read_leb_uint32(p, buf_end, import_count);
    LOG_VERBOSE("Component Import section with %u imports.", import_count);

    if (import_count == 0) {
        *p_buf = p;
        return true;
    }
    if (component->imports) {
        set_error_buf(error_buf, error_buf_size, "duplicate import section");
        goto fail;
    }
    component->imports = loader_malloc(
        import_count * sizeof(WASMComponentImport), error_buf, error_buf_size);
    if (!component->imports) {
        goto fail;
    }
    component->import_count = import_count;
    memset(component->imports, 0, import_count * sizeof(WASMComponentImport));

    for (i = 0; i < import_count; i++) {
        WASMComponentImport *current_import = &component->imports[i];
        if (!parse_string(&p, buf_end, &current_import->name, error_buf, error_buf_size)) {
            goto fail;
        }
        if (!parse_extern_desc(&p, buf_end, &current_import->desc, error_buf, error_buf_size)) {
            goto fail;
        }
        LOG_VERBOSE("Import %u: name '%s', desc_kind %u", i, current_import->name, current_import->desc.kind);
    }

    *p_buf = p;
    return true;
fail:
    return false;
}

static bool
load_export_section(const uint8 **p_buf, const uint8 *buf_end,
                    WASMComponent *component,
                    char *error_buf, uint32 error_buf_size)
{
    const uint8 *p = *p_buf;
    uint32 export_count, i;

    read_leb_uint32(p, buf_end, export_count);
    LOG_VERBOSE("Component Export section with %u exports.", export_count);

    if (export_count == 0) {
        *p_buf = p;
        return true;
    }
    if (component->exports) {
        set_error_buf(error_buf, error_buf_size, "duplicate export section");
        goto fail;
    }
    component->exports = loader_malloc(
        export_count * sizeof(WASMComponentExport), error_buf, error_buf_size);
    if (!component->exports) {
        goto fail;
    }
    component->export_count = export_count;
    memset(component->exports, 0, export_count * sizeof(WASMComponentExport));

    for (i = 0; i < export_count; i++) {
        WASMComponentExport *current_export = &component->exports[i];
        uint8 kind_byte;
        uint8 optional_type_present;

        if (!parse_string(&p, buf_end, &current_export->name, error_buf, error_buf_size)) {
            goto fail;
        }
        CHECK_BUF(p, buf_end, 1); /* kind byte */
        kind_byte = *p++;
        current_export->kind = (WASMComponentExportKind)kind_byte;
        read_leb_uint32(p, buf_end, current_export->item_idx);

        CHECK_BUF(p, buf_end, 1); /* optional type_idx presence byte */
        optional_type_present = *p++;
        if (optional_type_present == 0x01) {
            read_leb_uint32(p, buf_end, current_export->optional_desc_type_idx);
        } else {
            current_export->optional_desc_type_idx = (uint32)-1; /* Sentinel for not present */
        }
        LOG_VERBOSE("Export %u: name '%s', kind %u, item_idx %u, type_idx? %s (%u)",
                    i, current_export->name, current_export->kind, current_export->item_idx,
                    optional_type_present ? "yes" : "no", current_export->optional_desc_type_idx);
    }

    *p_buf = p;
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
    memset(component, 0, sizeof(WASMComponent)); /* Initialize all fields to 0/NULL */
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
            case COMPONENT_SECTION_ID_CORE_MODULE: /* ID 0 */
                if (!load_core_module_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail;
                break;
            case COMPONENT_SECTION_ID_CORE_INSTANCE: /* ID 1 */
                if (!load_core_instance_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            case COMPONENT_SECTION_ID_CORE_TYPE: /* ID 2 */
                 if (!load_core_type_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            case COMPONENT_SECTION_ID_COMPONENT: /* ID 3 */
                if (!load_component_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            case COMPONENT_SECTION_ID_INSTANCE: /* ID 4 */
                if (!load_instance_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            case COMPONENT_SECTION_ID_ALIAS: /* ID 5 */
                if (!load_alias_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            case COMPONENT_SECTION_ID_TYPE: /* ID 6 (Outer Type Section) */
                if (!load_type_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            case COMPONENT_SECTION_ID_DEFINED_TYPE: /* ID 7 (Component Defined Type Section - deftype) */
                if (!load_component_defined_type_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail;
                break;
            case COMPONENT_SECTION_ID_CANONICAL: /* ID 8 - Calls function from wasm_component_canonical.c */
                if (!load_canonical_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail;
                break;
            case COMPONENT_SECTION_ID_START: /* ID 9 */
                if (!load_start_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            case COMPONENT_SECTION_ID_IMPORT: /* ID 10 */
                if (!load_import_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            case COMPONENT_SECTION_ID_EXPORT: /* ID 11 */
                if (!load_export_section(&p, p + section_size, component, error_buf, error_buf_size)) goto fail; 
                break;
            default:
                os_printf("Skipping unknown component section ID: %u, size: %u\n", section_id, section_size);
                p += section_size; /* Advance pointer for unknown sections */
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

    if (component->core_instances) {
        for (uint32 i = 0; i < component->core_instance_count; i++) {
            WASMComponentCoreInstance *inst = &component->core_instances[i];
            if (inst->kind == CORE_INSTANCE_KIND_INSTANTIATE) {
                if (inst->u.instantiate.args) {
                    for (uint32 j = 0; j < inst->u.instantiate.arg_count; j++) {
                        if (inst->u.instantiate.args[j].name) {
                            wasm_runtime_free(inst->u.instantiate.args[j].name);
                        }
                    }
                    wasm_runtime_free(inst->u.instantiate.args);
                }
            } else if (inst->kind == CORE_INSTANCE_KIND_INLINE_EXPORT) {
                if (inst->u.inline_export.exports) {
                    for (uint32 j = 0; j < inst->u.inline_export.export_count; j++) {
                        if (inst->u.inline_export.exports[j].name) {
                            wasm_runtime_free(inst->u.inline_export.exports[j].name);
                        }
                    }
                    wasm_runtime_free(inst->u.inline_export.exports);
                }
            }
        }
        wasm_runtime_free(component->core_instances);
    }

    if (component->core_type_defs) { /* Simple structs, no deep free needed for now */
        wasm_runtime_free(component->core_type_defs);
    }

    if (component->nested_components) { /* Data points into original buffer, no deep free of component_data */
        /* If parsed_component was populated, it would need recursive unload */
        wasm_runtime_free(component->nested_components);
    }

    if (component->component_instances) {
        for (uint32 i = 0; i < component->component_instance_count; i++) {
            WASMComponentInstance *inst = &component->component_instances[i];
            if (inst->args) {
                for (uint32 j = 0; j < inst->arg_count; j++) {
                    if (inst->args[j].name) {
                        wasm_runtime_free(inst->args[j].name);
                    }
                }
                wasm_runtime_free(inst->args);
            }
        }
        wasm_runtime_free(component->component_instances);
    }

    if (component->aliases) {
        for (uint32 i = 0; i < component->alias_count; i++) {
            if (component->aliases[i].target_name) {
                wasm_runtime_free(component->aliases[i].target_name);
            }
        }
        wasm_runtime_free(component->aliases);
    }

    if (component->type_definitions) {
        for (uint32 i = 0; i < component->type_definition_count; ++i) {
            free_defined_type_contents(&component->type_definitions[i]);
        }
        wasm_runtime_free(component->type_definitions);
    }
    if (component->component_type_definitions) {
         for (uint32 i = 0; i < component->component_type_definition_count; ++i) {
            free_defined_type_contents(&component->component_type_definitions[i]);
        }
        wasm_runtime_free(component->component_type_definitions);
    }


    if (component->canonicals) {
        for (uint32 i = 0; i < component->canonical_count; i++) {
            if (component->canonicals[i].options) {
                wasm_runtime_free(component->canonicals[i].options);
            }
        }
        wasm_runtime_free(component->canonicals);
    }

    if (component->starts) { /* Assuming only one start function as per current parsing */
        if (component->starts->arg_value_indices) {
            wasm_runtime_free(component->starts->arg_value_indices);
        }
        wasm_runtime_free(component->starts);
    }

    if (component->imports) {
        for (uint32 i = 0; i < component->import_count; i++) {
            if (component->imports[i].name) {
                wasm_runtime_free(component->imports[i].name);
            }
            free_extern_desc_contents(&component->imports[i].desc);
        }
        wasm_runtime_free(component->imports);
    }

    if (component->exports) {
        for (uint32 i = 0; i < component->export_count; i++) {
            if (component->exports[i].name) {
                wasm_runtime_free(component->exports[i].name);
            }
            // Export desc is not directly stored, but item_idx and optional_desc_type_idx
        }
        wasm_runtime_free(component->exports);
    }

    wasm_runtime_free(component);
}
