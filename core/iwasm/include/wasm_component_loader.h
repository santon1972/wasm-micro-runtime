/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_COMPONENT_LOADER_H
#define _WASM_COMPONENT_LOADER_H

#include "wasm_component.h"
#include "wasm_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMPONENT_MODEL_LAYER_PRIMARY 0x01 /* Primary layer, replacing 0x01 for clarity */

typedef enum WASMComponentTypeKind {
    COMPONENT_TYPE_KIND_MODULE = 0,
    COMPONENT_TYPE_KIND_COMPONENT = 1,
    COMPONENT_TYPE_KIND_INSTANCE = 2,
    COMPONENT_TYPE_KIND_FUNCTION = 3,
    COMPONENT_TYPE_KIND_VALUE = 4,
    COMPONENT_TYPE_KIND_TYPE = 5,
    COMPONENT_TYPE_KIND_ENUM = 6,
    COMPONENT_TYPE_KIND_UNION = 7,
    COMPONENT_TYPE_KIND_RECORD = 8,
    COMPONENT_TYPE_KIND_VARIANT = 9,
    COMPONENT_TYPE_KIND_LIST = 10,
    COMPONENT_TYPE_KIND_TUPLE = 11,
    COMPONENT_TYPE_KIND_FLAGS = 12,
    COMPONENT_TYPE_KIND_OWN = 13,
    COMPONENT_TYPE_KIND_BORROW = 14,
    COMPONENT_TYPE_KIND_CORE_TYPE = 15, /* Not in spec, internal use */
} WASMComponentTypeKind;

typedef enum WASMComponentCoreTypeKind {
    WASM_COMPONENT_CORE_FUNC_TYPE_KIND = 0x40,
    CORE_TYPE_KIND_TABLE = 0x4F, /* Chosen to be distinct */
    CORE_TYPE_KIND_MEMORY = 0x4E  /* Chosen to be distinct */
} WASMComponentCoreTypeKind;

typedef struct WASMComponentValType {
    /* Can be WASM_TYPE_xxx for primitive types like I32, F32, etc.
       or COMPONENT_TYPE_KIND_xxx for component model defined types */
    uint8 kind;
    /* If kind is one of COMPONENT_TYPE_KIND_xxx,
       this is the index into WASMComponent->defined_types */
    uint32 type_idx;
} WASMComponentValType;

typedef struct WASMComponentCoreFuncType {
    uint32 param_count;
    /* types are WASMComponentValType, but encoded as wasm core types */
    uint8 *param_types; /* array of wasm core value types */
    uint32 result_count;
    /* types are WASMComponentValType, but encoded as wasm core types */
    uint8 *result_types; /* array of wasm core value types */
} WASMComponentCoreFuncType;

/* Corresponds to core wasm table_type */
typedef struct WASMComponentCoreTableType {
    uint8 elem_type;  /* val_type, e.g. REF_NULL_FUNCREF, REF_NULL_EXTERNREF */
    uint8 limits_flags; /* 0x01: has_max_size */
    uint32 init_size;
    uint32 max_size;   /* only valid if has_max_size is true */
} WASMComponentCoreTableType;

/* Corresponds to core wasm memory_type */
typedef struct WASMComponentCoreMemoryType {
    uint8 limits_flags; /* 0x01: has_max_size, 0x02: is_shared */
    uint32 init_page_count;
    uint32 max_page_count; /* only valid if has_max_size is true */
} WASMComponentCoreMemoryType;

typedef union WASMComponentCoreTypeDef {
    WASMComponentCoreFuncType func_type;
    WASMComponentCoreTableType table_type;
    WASMComponentCoreMemoryType memory_type;
    /* Possible other core types like module, instance in the future */
} WASMComponentCoreTypeDef;

typedef struct WASMComponentDefinedType {
    uint8 kind; /* WASMComponentTypeKind or WASMComponentCoreTypeKind */
    union {
        WASMComponentType component_type; /* If kind == COMPONENT_TYPE_KIND_COMPONENT */
        WASMModuleType module_type;       /* If kind == COMPONENT_TYPE_KIND_MODULE */
        WASMFunctionType func_type;     /* If kind == COMPONENT_TYPE_KIND_FUNCTION */
                                        /* (for component functions) */
        WASMComponentCoreTypeDef core_type_def; /* If kind is a core type kind */
        /* TODO: Add other component type structures here (enum, record, variant, etc.) */
        /* For now, we are focusing on core types and functions */
        struct { /* For COMPONENT_TYPE_KIND_VALUE */
            WASMComponentValType val_type;
        } value;
        struct { /* For COMPONENT_TYPE_KIND_TYPE (referencing another type) */
            uint32 type_idx;
        } type_ref;

        /* Placeholder for other defined types like record, variant, tuple, flags, enum, union, list */
        /* Example for Record */
        /* struct {
            uint32 field_count;
            String* field_names;
            WASMComponentValType* field_types;
        } record_type; */

    } u;
} WASMComponentDefinedType;

WASMComponent*
wasm_component_load(const uint8 *buf, uint32 size, char *error_buf,
                    uint32 error_buf_size);

void
wasm_component_destroy(WASMComponent *component);

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _WASM_COMPONENT_LOADER_H */
