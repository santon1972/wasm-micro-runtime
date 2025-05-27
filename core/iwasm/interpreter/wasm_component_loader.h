/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_COMPONENT_LOADER_H
#define _WASM_COMPONENT_LOADER_H

#include "wasm.h"
#include "../common/wasm_component.h" /* For COMPONENT_MODEL_VERSION, etc. */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct WASMComponent;

/* Section Type IDs for Component Model (from Binary.md) */
#define COMPONENT_SECTION_ID_CORE_MODULE    0
#define COMPONENT_SECTION_ID_CORE_INSTANCE  1
#define COMPONENT_SECTION_ID_CORE_TYPE      2
#define COMPONENT_SECTION_ID_COMPONENT      3
#define COMPONENT_SECTION_ID_INSTANCE       4
#define COMPONENT_SECTION_ID_ALIAS          5
#define COMPONENT_SECTION_ID_TYPE           6
#define COMPONENT_SECTION_ID_CANONICAL      7
#define COMPONENT_SECTION_ID_START          8
#define COMPONENT_SECTION_ID_IMPORT         9
#define COMPONENT_SECTION_ID_EXPORT         10
/* Custom section ID is 0 for core wasm, but component custom sections
   are distinct. Binary.md uses 11 for "value" which might be a typo
   and meant to be a specific section, or perhaps a general custom section.
   For now, I'll assume specific sections as listed.
   If there's a generic component custom section, its ID would be different
   from core WASM's custom section ID.
   The "Value Section" is not explicitly listed with an ID in Binary.md's
   section overview, but is described later. I'll hold off on this one
   until I have more clarity or if it's part of another section's parsing.
*/


/* Structure for a Core Module Section item */
typedef struct WASMComponentCoreModule {
    uint32 module_len; /* length of the core wasm module data */
    uint8 *module_data; /* pointer to the core wasm module data (within component buffer) */
    WASMModule *module_object; /* loaded WASMModule structure */
    /* Potentially more fields if we parse compile-time options, etc. */
} WASMComponentCoreModule;

/* Argument for instantiating a core module */
typedef struct WASMComponentCoreInstanceArg {
    char *name;       /* name of the import */
    uint32 instance_idx; /* index of the item (e.g., func, table, memory, global)
                            provided for the import. This could be an alias
                            or an item from another instance. */
    /* uint8 kind; TODO: to specify what kind of item instance_idx refers to, if needed beyond name */
} WASMComponentCoreInstanceArg;

/* Inline export from a core module instance */
typedef struct WASMComponentCoreInlineExport {
    char *name;    /* name of the export */
    uint8 kind;    /* wasm_export_kind_t: func, table, memory, global */
    uint32 sort_idx; /* index of the export in the core module */
} WASMComponentCoreInlineExport;

/* Structure for a Core Instance Section item */
typedef enum WASMCoreInstanceKind {
    CORE_INSTANCE_KIND_INSTANTIATE = 0x00,
    CORE_INSTANCE_KIND_INLINE_EXPORT = 0x01
} WASMCoreInstanceKind;

typedef struct WASMComponentCoreInstance {
    WASMCoreInstanceKind kind;
    union {
        struct {
            uint32 module_idx; /* index to a core module section */
            uint32 arg_count;
            WASMComponentCoreInstanceArg *args;
        } instantiate;
        struct {
            uint32 export_count;
            WASMComponentCoreInlineExport *exports;
        } inline_export;
    } u;
} WASMComponentCoreInstance;

/* Structure for a Core Type Section item (e.g. core function types) */
typedef struct WASMComponentCoreType {
    /* TODO: Define structures for core function types, module types, etc. */
    /* For now, just a placeholder */
    uint8 type_tag; /* e.g., 0x60 for func type */
} WASMComponentCoreType;

/* Structure for a Component Section item (nested component) */
typedef struct WASMComponentNestedComponent {
    uint32 component_len;
    uint8 *component_data;
} WASMComponentNestedComponent;

/* Structure for an Instance Section item (Instantiation of a component or core module) */
typedef struct WASMComponentInstance {
    uint8 instance_kind; /* 0x00 for core module instance, 0x01 for component instance */
    uint32 item_idx;     /* index to a core module or component section */
    uint32 arg_count;
    /* TODO: Define structures for instance arguments */
    /* WASMInstanceArg *args; */
} WASMComponentInstance;

/* Primitive Value Types */
typedef enum WASMComponentPrimValType {
    PRIM_VAL_BOOL,
    PRIM_VAL_S8,
    PRIM_VAL_U8,
    PRIM_VAL_S16,
    PRIM_VAL_U16,
    PRIM_VAL_S32,
    PRIM_VAL_U32,
    PRIM_VAL_S64,
    PRIM_VAL_U64,
    PRIM_VAL_F32,
    PRIM_VAL_F64,
    PRIM_VAL_CHAR,
    PRIM_VAL_STRING
} WASMComponentPrimValType;

/* Forward declaration for recursive ValType */
struct WASMComponentValType;

typedef struct WASMComponentLabelValType {
    char *label;
    struct WASMComponentValType *valtype;
} WASMComponentLabelValType;

typedef struct WASMComponentRecordType {
    uint32 field_count;
    WASMComponentLabelValType *fields;
} WASMComponentRecordType;

typedef struct WASMComponentCase {
    char *label;
    struct WASMComponentValType *valtype; /* Optional, NULL if not present */
    uint32 default_case_idx; /* Index to another case if this is a default, or special value */
} WASMComponentCase;

typedef struct WASMComponentVariantType {
    uint32 case_count;
    WASMComponentCase *cases;
} WASMComponentVariantType;

typedef struct WASMComponentListType {
    struct WASMComponentValType *element_valtype;
} WASMComponentListType;

typedef struct WASMComponentTupleType {
    uint32 element_count;
    struct WASMComponentValType *element_valtypes;
} WASMComponentTupleType;

typedef struct WASMComponentFlagsType {
    uint32 label_count;
    char **labels;
} WASMComponentFlagsType;

typedef struct WASMComponentEnumType {
    uint32 label_count;
    char **labels;
} WASMComponentEnumType;

typedef enum WASMComponentValTypeKind {
    VAL_TYPE_KIND_PRIMITIVE,
    VAL_TYPE_KIND_RECORD,
    VAL_TYPE_KIND_VARIANT,
    VAL_TYPE_KIND_LIST,
    VAL_TYPE_KIND_TUPLE,
    VAL_TYPE_KIND_FLAGS,
    VAL_TYPE_KIND_ENUM,
    VAL_TYPE_KIND_OPTION,
    VAL_TYPE_KIND_RESULT,
    VAL_TYPE_KIND_OWN_TYPE_IDX,    /* Own a resource, stores typeidx to a resource type */
    VAL_TYPE_KIND_BORROW_TYPE_IDX, /* Borrow a resource, stores typeidx to a resource type */
    VAL_TYPE_KIND_TYPE_IDX         /* Reference to another defined type in the same component type section */
} WASMComponentValTypeKind;

typedef struct WASMComponentOptionType {
    struct WASMComponentValType *valtype; /* NULL if not present (for `option<void>`) */
} WASMComponentOptionType;

typedef struct WASMComponentResultType {
    struct WASMComponentValType *ok_valtype;  /* NULL if not present (e.g. `result<_, E>`) */
    struct WASMComponentValType *err_valtype; /* NULL if not present (e.g. `result<T, _>`) */
} WASMComponentResultType;

typedef struct WASMComponentValType {
    WASMComponentValTypeKind kind;
    union {
        WASMComponentPrimValType primitive;
        WASMComponentRecordType record;
        WASMComponentVariantType variant;
        WASMComponentListType list;
        WASMComponentTupleType tuple;
        WASMComponentFlagsType flags;
        WASMComponentEnumType enum_type;
        WASMComponentOptionType option;
        WASMComponentResultType result;
        uint32 type_idx; /* For VAL_TYPE_KIND_TYPE_IDX, VAL_TYPE_KIND_OWN_TYPE_IDX, VAL_TYPE_KIND_BORROW_TYPE_IDX */
    } u;
} WASMComponentValType;

/* Alias target kinds */
typedef enum WASMAliasTargetKind {
    ALIAS_TARGET_CORE_EXPORT = 0x00, /* (core export i n) */
    ALIAS_TARGET_CORE_MODULE = 0x01, /* (core module i) - for future use */
    ALIAS_TARGET_TYPE = 0x02,        /* (type i) - for future use */
    ALIAS_TARGET_COMPONENT = 0x03,   /* (component i) - for future use */
    ALIAS_TARGET_INSTANCE = 0x04,    /* (instance i) - for future use */
} WASMAliasTargetKind;

/* Sorts for aliases (wasm_kind from Binary.md) */
typedef enum WASMAliasSort {
    ALIAS_SORT_CORE_FUNC = 0x00,
    ALIAS_SORT_CORE_TABLE = 0x01,
    ALIAS_SORT_CORE_MEMORY = 0x02,
    ALIAS_SORT_CORE_GLOBAL = 0x03,
    ALIAS_SORT_CORE_MODULE = 0x04, /* When aliasing a core module itself */
    ALIAS_SORT_TYPE = 0x05,
    ALIAS_SORT_COMPONENT = 0x06,
    /* ALIAS_SORT_INSTANCE, ALIAS_SORT_VALUE etc. can be added */
} WASMAliasSort;


/* Structure for an Alias Section item */
typedef struct WASMComponentAlias {
    WASMAliasSort sort; /* The kind of item being aliased */
    WASMAliasTargetKind target_kind;
    union {
        struct {
            uint32 core_instance_idx; /* index into component->core_instances */
            char *name;               /* name of the export from the core instance */
        } core_export;
        /* Other alias targets can be added here */
        uint32 outer_idx; /* For outer aliases (future) */
        uint32 type_idx;  /* For type aliases (future) */
    } target;
    /* uint32 new_item_idx;  The new index for the aliased item, implicit by order in section?
                              Or part of the alias instruction if it creates a new index in current scope.
                              Binary.md suggests alias creates a new item in the current scope's index space.
                              For now, the position in the aliases array will be its index.
    */
} WASMComponentAlias;

/* Renamed from WASMComponentType to avoid conflict.
   This represents an entry in the Type Section (ID 6), which defines
   component types, instance types, etc.
   For this subtask, we are primarily focused on the *internal structure* of
   value types, which will be defined within the Component Type Section (ID 7)
   via `defvaltype`.
*/
typedef struct WASMComponentSectionTypeEntry {
    /* TODO: Define structures for various component types (component, instance, resource) */
    /* For now, just a placeholder */
    uint8 type_tag; /* e.g., component type (0x40), instance type (0x41), resource type (0x42) */
} WASMComponentSectionTypeEntry;


/* Represents an entry in the Component Type Section (ID 7) */
typedef enum WASMComponentDefinedTypeKind {
    DEF_TYPE_KIND_VALTYPE = 0x00,     /* defvaltype */
    DEF_TYPE_KIND_FUNC = 0x01,        /* functype */
    DEF_TYPE_KIND_COMPONENT = 0x02,   /* componenttype */
    DEF_TYPE_KIND_INSTANCE = 0x03,    /* instancetype */
    DEF_TYPE_KIND_RESOURCE = 0x04,    /* resourcetype */
    DEF_TYPE_KIND_CORE_MODULE = 0x05, /* core:module type - for future use */
} WASMComponentDefinedTypeKind;

/* Note: Binary.md uses 0x40 for functype tag in type definition encoding.
   The DEF_TYPE_KIND_FUNC (0x01) is for the deftype vector.
*/
typedef struct WASMComponentFuncType {
    uint32 param_count;
    WASMComponentLabelValType *params; /* <label, valtype> pairs */
    WASMComponentValType *result;      /* Optional: NULL if no result (void) */
} WASMComponentFuncType;

typedef struct WASMComponentDefinedType {
    WASMComponentDefinedTypeKind kind;
    union {
        WASMComponentValType valtype;
        WASMComponentFuncType func_type;
        struct WASMComponentComponentType *comp_type; /* Pointer to allow recursive definition if needed */
        struct WASMComponentInstanceType *inst_type; /* Pointer for similar reasons */
        struct WASMComponentResourceType *res_type;
    } u;
} WASMComponentDefinedType;

/* ExternDesc and related types */
typedef enum WASMComponentExternDescKind {
    EXTERN_DESC_KIND_MODULE = 0x00,
    EXTERN_DESC_KIND_FUNC = 0x01,
    EXTERN_DESC_KIND_VALUE = 0x02,
    EXTERN_DESC_KIND_TYPE = 0x03,
    EXTERN_DESC_KIND_INSTANCE = 0x04,
    EXTERN_DESC_KIND_COMPONENT = 0x05
} WASMComponentExternDescKind;

typedef enum WASMComponentTypeBoundKind {
    TYPE_BOUND_KIND_EQ = 0x00,
    TYPE_BOUND_KIND_SUB_RESOURCE = 0x01
} WASMComponentTypeBoundKind;

typedef struct WASMComponentTypeBound {
    WASMComponentTypeBoundKind kind;
    union {
        uint32 type_idx; /* For EQ */
        /* No specific data for SUB_RESOURCE for now */
    } u;
} WASMComponentTypeBound;

typedef struct WASMComponentExternDesc {
    WASMComponentExternDescKind kind;
    union {
        uint32 core_module_type_idx; /* For EXTERN_DESC_KIND_MODULE (refers to a core:module type) */
        uint32 func_type_idx;        /* For EXTERN_DESC_KIND_FUNC (refers to a component functype) */
        WASMComponentValType *value_type; /* For EXTERN_DESC_KIND_VALUE */
        WASMComponentTypeBound type_bound; /* For EXTERN_DESC_KIND_TYPE */
        uint32 instance_type_idx;    /* For EXTERN_DESC_KIND_INSTANCE (refers to an instancetype) */
        uint32 component_type_idx;   /* For EXTERN_DESC_KIND_COMPONENT (refers to a componenttype) */
    } u;
} WASMComponentExternDesc;

/* Import/Export Declarations */
typedef struct WASMComponentImportDecl {
    char *name;
    WASMComponentExternDesc desc;
} WASMComponentImportDecl;

typedef struct WASMComponentExportDecl {
    char *name;
    WASMComponentExternDesc desc;
    /* uint32 optional_type_idx; // If there's an explicit type annotation for the export */
} WASMComponentExportDecl;

/* ComponentType and InstanceType Declarations */
typedef enum WASMComponentDeclKind {
    COMPONENT_DECL_KIND_IMPORT = 0x00,
    COMPONENT_DECL_KIND_CORE_TYPE = 0x01, /* core:type i t */
    COMPONENT_DECL_KIND_TYPE = 0x02,      /* type i t */
    COMPONENT_DECL_KIND_ALIAS = 0x03,     /* alias ... */
    COMPONENT_DECL_KIND_EXPORT = 0x04,    /* export ... */
    /* Potentially others like core:instance, instance, etc., if considered decls */
} WASMComponentDeclKind;

typedef struct WASMComponentDecl {
    WASMComponentDeclKind kind;
    union {
        WASMComponentImportDecl import_decl;
        /* TODO: Define structs for core_type_decl, type_decl, alias_decl, export_decl */
        /* For now, can use placeholder or skip detailed parsing within these stubs */
        uint32 placeholder_idx; /* Placeholder for other decl kinds */
    } u;
} WASMComponentDecl;

typedef enum WASMComponentInstanceDeclKind {
    INSTANCE_DECL_KIND_CORE_TYPE = 0x01, /* core:type i t */
    INSTANCE_DECL_KIND_TYPE = 0x02,      /* type i t */
    INSTANCE_DECL_KIND_ALIAS = 0x03,     /* alias ... */
    INSTANCE_DECL_KIND_EXPORT = 0x04     /* export ... */
} WASMComponentInstanceDeclKind;

typedef struct WASMComponentInstanceDecl {
    WASMComponentInstanceDeclKind kind;
    union {
        /* TODO: Define structs for core_type_decl, type_decl, alias_decl, export_decl */
        uint32 placeholder_idx; /* Placeholder for other decl kinds */
    } u;
} WASMComponentInstanceDecl;

/* ComponentType, InstanceType, ResourceType Definitions */
typedef struct WASMComponentComponentType {
    uint32 decl_count;
    WASMComponentDecl *decls;
} WASMComponentComponentType;

typedef struct WASMComponentInstanceType {
    uint32 decl_count;
    WASMComponentInstanceDecl *decls;
} WASMComponentInstanceType;

typedef struct WASMComponentResourceType {
    uint32 rep; /* Expected to be 0x7F (i32) for now */
    uint32 dtor_func_idx; /* (uint32)-1 if not present */
} WASMComponentResourceType;


/* Structure for a Canonical Section item (Canonical ABI functions) */
typedef struct WASMComponentCanonical {
    uint8 func_kind; /* e.g., lift, lower */
    uint32 core_func_idx;
    uint32 component_type_idx;
    /* TODO: Define structures for canonical options */
} WASMComponentCanonical;

/* Structure for a Start Section item (Component Start Function) */
typedef struct WASMComponentStart {
    uint32 func_idx; /* index to a component function */
    uint32 arg_count;
    /* TODO: Define structures for start function arguments */
    /* WASMValue *args; */
} WASMComponentStart;

/* Structure for an Import Section item (Component Imports) */
typedef struct WASMComponentImport {
    char *name; /* import name */
    uint32 type_idx; /* index to a component type section */
    /* TODO: Define structures for import descriptions (url, hash, etc.) */
} WASMComponentImport;

/* Structure for an Export Section item (Component Exports) */
typedef struct WASMComponentExport {
    char *name; /* export name */
    uint8 kind; /* e.g., func, component, instance, type */
    uint32 item_idx;
    uint32 type_idx; /* optional type index for typed exports */
} WASMComponentExport;


/* Main structure representing a parsed WASM Component */
typedef struct WASMComponent {
    uint32 version;
    uint32 layer; /* Should be 1 for current spec */

    /* Arrays of component sections */
    WASMComponentCoreModule *core_modules;
    uint32 core_module_count;

    WASMComponentCoreInstance *core_instances;
    uint32 core_instance_count;

    WASMComponentCoreType *core_types;
    uint32 core_type_count;

    WASMComponentNestedComponent *components;
    uint32 component_count;

    WASMComponentInstance *instances;
    uint32 instance_count;

    WASMComponentAlias *aliases;
    uint32 alias_count;

    WASMComponentSectionTypeEntry *section_types; /* Entries from Type Section (ID 6) */
    uint32 section_type_count;

    WASMComponentDefinedType *defined_types; /* Entries from Component Type Section (ID 7) */
    uint32 defined_type_count;

    WASMComponentCanonical *canonicals;
    uint32 canonical_count;

    WASMComponentStart *starts;
    uint32 start_count;

    WASMComponentImport *imports;
    uint32 import_count;

    WASMComponentExport *exports;
    uint32 export_count;

    /* TODO: Add other fields as needed, e.g., for custom sections, memory management */

} WASMComponent;


/* Function to load a WASM component from a buffer */
WASMComponent*
wasm_component_load(const uint8 *buf, uint32 size, char *error_buf, uint32 error_buf_size);

/* Function to unload a WASM component and free its resources */
void
wasm_component_unload(WASMComponent *component);

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _WASM_COMPONENT_LOADER_H */
