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

/* Section Type IDs for Component Model (Component Model 1.0 Spec) */
/* Custom section ID is 0 (same as core WASM) */
#define COMPONENT_SECTION_ID_CORE_MODULE    1  /* MVP Spec: core module */
#define COMPONENT_SECTION_ID_CORE_INSTANCE  2  /* MVP Spec: core instance */
#define COMPONENT_SECTION_ID_CORE_TYPE      3  /* MVP Spec: core type */
#define COMPONENT_SECTION_ID_COMPONENT      4  /* MVP Spec: component */
#define COMPONENT_SECTION_ID_INSTANCE       5  /* MVP Spec: instance */
#define COMPONENT_SECTION_ID_ALIAS          6  /* MVP Spec: alias */
#define COMPONENT_SECTION_ID_TYPE           7  /* MVP Spec: type (defines all component types: functype, componenttype, instancetype, resourcetype, valtype) */
                                               /* This merges WAMR's previous ID 6 and 7 */
#define COMPONENT_SECTION_ID_CANONICAL      8  /* MVP Spec: canonical function */
#define COMPONENT_SECTION_ID_START          9  /* MVP Spec: start function */
#define COMPONENT_SECTION_ID_IMPORT         10 /* MVP Spec: import */
#define COMPONENT_SECTION_ID_EXPORT         11 /* MVP Spec: export */
#define COMPONENT_SECTION_ID_VALUE          12 /* MVP Spec: value section (for constant values) */
/* Note: COMPONENT_SECTION_ID_DEFINED_TYPE (WAMR's old 7) is removed, functionality merged into COMPONENT_SECTION_ID_TYPE (new 7) */


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
    uint8 kind;       /* kind of the import (e.g., WASM_EXTERNAL_FUNCTION from wasm.h), derived during loading by looking up 'name' in the target core module's imports. */
    uint32 instance_idx; /* index of the item (e.g., func, table, memory, global)
                            provided for the import. This could be an alias
                            or an item from another instance. */
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

/* Forward declarations for types used in WASMComponentCoreTypeDef */
struct WASMComponentCoreFuncType;
struct WASMComponentCoreModuleType;

/* Represents a core function type (param and result types) */
typedef struct WASMComponentCoreFuncType {
    uint32 param_count;
    uint8 *param_types; /* Array of core value types (e.g., VALUE_TYPE_I32 from wasm.h) */
    uint32 result_count;
    uint8 *result_types; /* Array of core value types */
} WASMComponentCoreFuncType;

/* Represents an individual export of a core module type */
typedef struct WASMComponentCoreModuleExport {
    char *name;
    uint8 kind; /* e.g., WASM_EXTERNAL_FUNCTION, WASM_EXTERNAL_TABLE, etc. from wasm.h */
    uint32 type_idx; /* Index to a WASMComponentCoreTypeDef (e.g., for function signature)
                        or other type description as applicable. For non-func kinds,
                        this might need further refinement or a union if detailed
                        type info (beyond kind) is stored directly. */
} WASMComponentCoreModuleExport;

/* Represents an individual import of a core module type */
typedef struct WASMComponentCoreModuleImport {
    char *module_name;
    char *field_name;
    uint8 kind; /* e.g., WASM_EXTERNAL_FUNCTION, WASM_EXTERNAL_TABLE, etc. from wasm.h */
    uint32 type_idx; /* Index to a WASMComponentCoreTypeDef (e.g., for function signature)
                        or other type description. Similar to export, non-func kinds
                        might need more detail here. */
} WASMComponentCoreModuleImport;

/* Represents a core module type, detailing its imports and exports */
typedef struct WASMComponentCoreModuleType {
    uint32 import_count;
    WASMComponentCoreModuleImport *imports;
    uint32 export_count;
    WASMComponentCoreModuleExport *exports;
} WASMComponentCoreModuleType;

/* Define for the new core module type kind */
#define CORE_TYPE_KIND_MODULE 0x50

/* Structure for a Core Type Section item (e.g. core function types, core module types) */
/* Represents a core:type definition */
typedef struct WASMComponentCoreTypeDef {
    uint8 kind; /* e.g., 0x60 for core func type, CORE_TYPE_KIND_MODULE for core module type */
    union {
        struct WASMComponentCoreFuncType *core_func_type; /* For core function type (kind == 0x60) */
        WASMComponentCoreModuleType *module_type;       /* For core module type (kind == CORE_TYPE_KIND_MODULE) */
        /* Add other core type structures as needed */
    } u;
} WASMComponentCoreTypeDef;

/* Structure for a Component Section item (nested component) */
typedef struct WASMComponentNestedComponent {
    uint32 component_len;
    uint8 *component_data; /* Pointer to the raw component data */
    struct WASMComponent *parsed_component; /* Optional: if pre-parsed */
} WASMComponentNestedComponent;

/* Argument for instantiating a component instance or core module instance */
typedef enum WASMComponentInstanceArgKind {
    INSTANCE_ARG_KIND_VALUE,    /* (value <valueidx>) - Not directly in spec for component instance args, but for start func */
                                /* For component instance, it's (export <name> (value <valueidx>)) which is more complex */
    INSTANCE_ARG_KIND_EXPORT,   /* (instance <instanceidx>) (export <name>) ... or just (export <name>) referencing outer scope */
                                /* Let's simplify for now: item_idx refers to an existing item in the component's scope */
    INSTANCE_ARG_KIND_ITEM_IDX  /* Refers to an item (func, component, instance, type, value) by its index */
} WASMComponentInstanceArgKind;

typedef struct WASMComponentInstanceArg {
    char *name;       /* name of the argument (import name) */
    WASMComponentInstanceArgKind kind; /* Kind of item being passed as argument */
    uint8 actual_sort; /* The actual sort byte read from binary (func=0, value=1, type=2, component=3, instance=4) */
    uint32 item_idx;  /* index of the item (e.g., func, component, instance, type, value)
                         in the current component's context, or from an outer context via alias.
                         For core module instantiation, this is the wasm_item_t (func, table, mem, global)
                         from an existing core instance. */
} WASMComponentInstanceArg;

/* Structure for an Instance Section item (Instantiation of a component or core module) */
typedef struct WASMComponentInstance {
    /* Based on `instance` production in Binary.md:
       (instance (instantiate (component <compidx>) (with (vec (name string) (item <itemidx>)))))
       (instance (instantiate (core:module <modidx>) (with (vec (name string) (item <coreitemidx>)))))
    */
    uint8 instance_kind; /* 0x00 for core module instance, 0x01 for component instance */
    uint32 item_idx;     /* index to a core module section OR component section */
    uint32 arg_count;
    WASMComponentInstanceArg *args;
} WASMComponentInstance;

/* Primitive Value Types (Component Model 1.0 Spec Tags) */
typedef enum WASMComponentPrimValType {
    PRIM_VAL_BOOL   = 0x7f, /* bool */
    PRIM_VAL_S8     = 0x7e, /* s8 */
    PRIM_VAL_U8     = 0x7d, /* u8 */
    PRIM_VAL_S16    = 0x7c, /* s16 */
    PRIM_VAL_U16    = 0x7b, /* u16 */
    PRIM_VAL_S32    = 0x7a, /* s32 */
    PRIM_VAL_U32    = 0x79, /* u32 */
    PRIM_VAL_S64    = 0x78, /* s64 */
    PRIM_VAL_U64    = 0x77, /* u64 */
    PRIM_VAL_F32    = 0x76, /* f32 */
    PRIM_VAL_F64    = 0x75, /* f64 */
    PRIM_VAL_CHAR   = 0x74, /* char */
    PRIM_VAL_STRING = 0x73  /* string */
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

/* Defines the internal kind of a WASMComponentValType.
 * The loader maps Component Model 1.0 spec binary opcodes to these kinds.
 * Spec opcodes for `deftype` kinds that these map to:
 *   Record:   0x72
 *   Variant:  0x71
 *   List:     0x70 (Note: spec also has fixed-size list 0x67)
 *   Tuple:    0x6f
 *   Flags:    0x6e
 *   Enum:     0x6d
 *   Option:   0x6b
 *   Result:   0x6a
 *   Own:      0x69 (typeidx)
 *   Borrow:   0x68 (typeidx)
 * Primitive types (bool, s8, etc.) have their own direct tags (0x7f-0x73).
 * VAL_TYPE_KIND_TYPE_IDX is for internal references to already defined types.
 */
typedef enum WASMComponentValTypeKind {
    VAL_TYPE_KIND_PRIMITIVE,       /* Corresponds to PRIM_VAL_* tags */
    VAL_TYPE_KIND_RECORD,          /* Parsed from spec opcode 0x72 */
    VAL_TYPE_KIND_VARIANT,         /* Parsed from spec opcode 0x71 */
    VAL_TYPE_KIND_LIST,            /* Parsed from spec opcode 0x70 */
    VAL_TYPE_KIND_TUPLE,           /* Parsed from spec opcode 0x6f */
    VAL_TYPE_KIND_FLAGS,           /* Parsed from spec opcode 0x6e */
    VAL_TYPE_KIND_ENUM,            /* Parsed from spec opcode 0x6d */
    VAL_TYPE_KIND_OPTION,          /* Parsed from spec opcode 0x6b */
    VAL_TYPE_KIND_RESULT,          /* Parsed from spec opcode 0x6a */
    VAL_TYPE_KIND_OWN_TYPE_IDX,    /* Parsed from spec opcode 0x69, stores typeidx to a resource type */
    VAL_TYPE_KIND_BORROW_TYPE_IDX, /* Parsed from spec opcode 0x68, stores typeidx to a resource type */
    VAL_TYPE_KIND_TYPE_IDX         /* Reference to another defined type (valtype) in a type section (internal use) */
} WASMComponentValTypeKind;

typedef struct WASMComponentOptionType {
    struct WASMComponentValType *valtype;
} WASMComponentOptionType;

typedef struct WASMComponentResultType {
    struct WASMComponentValType *ok_valtype;  /* NULL for result<_, E> */
    struct WASMComponentValType *err_valtype; /* NULL for result<T, _> */
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
    ALIAS_SORT_INSTANCE = 0x07,    /* Instance of a component */
    ALIAS_SORT_VALUE = 0x08        /* A value (e.g. from a value section, or a func producing a value) */
    /* Potentially other sorts can be added */
} WASMAliasSort;


/* Structure for an Alias Section item */
typedef struct WASMComponentAlias {
    WASMAliasSort sort; /* The kind of item being aliased */
    WASMAliasTargetKind target_kind; /* e.g. core export, outer, type */
    uint32 target_outer_depth;  /* For ALIAS_TARGET_OUTER: depth of outer component */
    uint32 target_idx;          /* Index into the target's item space (e.g. core_instance_idx, component_idx, type_idx) */
    char *target_name;          /* Optional: name of export if target_kind is CORE_EXPORT or similar */
    /* The alias itself creates a new item in the current component's index space for its sort. */
} WASMComponentAlias;

/*
  A type-def can be:
  - valtype (defined previously)
  - functype
  - componenttype
  - instancetype
  - resourcetype
  - core:module type (future)
  - core:functype (future)
  etc.

  WASMComponentDefinedType represents these definitions, typically found in
  Type Section (ID 6) or Component Type Section (ID 7).
*/

/* Forward declarations for recursive type definitions */
struct WASMComponentComponentType;
struct WASMComponentInstanceType;
struct WASMComponentResourceType;

/* Represents a component function type.
 * Spec `functype`: paramlist:<labelvaltype*> resultlist:(0x00 <valtype> | 0x01 0x00)
 * WAMR's result representation needs to align with this encoding during load/unload.
 */
typedef struct WASMComponentFuncType {
    uint32 param_count;
    WASMComponentLabelValType *params; /* <label, valtype> pairs */
    WASMComponentValType *result;      /* valtype for the result. NULL if spec is (0x01 0x00) for no result. */
} WASMComponentFuncType;

/* Represents a resource type definition */
typedef struct WASMComponentResourceType {
    uint32 rep; /* Core wasm type for representation, e.g., VAL_TYPE_I32 */
    uint32 dtor_func_idx; /* Optional: index to a core function destructor. (uint32)-1 if not present. */
} WASMComponentResourceType;


/* Represents an entry in the Type Section (ID 6) or Component Type Section (ID 7) */
typedef enum WASMComponentDefinedTypeKind {
    DEF_TYPE_KIND_VALTYPE = 0x00,     /* A ValType definition */
    DEF_TYPE_KIND_FUNC = 0x01,        /* A Component Function Type */
    DEF_TYPE_KIND_COMPONENT = 0x02,   /* A Component Type */
    DEF_TYPE_KIND_INSTANCE = 0x03,    /* An Instance Type */
    DEF_TYPE_KIND_RESOURCE = 0x04,    /* A Resource Type */
    DEF_TYPE_KIND_CORE_MODULE = 0x05, /* A Core Module Type (e.g. imports/exports of a core module) */
    /* Potentially others like core func type if explicitly defined here */
} WASMComponentDefinedTypeKind;


typedef struct WASMComponentDefinedType {
    WASMComponentDefinedTypeKind kind;
    union {
        WASMComponentValType valtype;
        WASMComponentFuncType func_type;
        struct WASMComponentComponentType *comp_type;
        struct WASMComponentInstanceType *inst_type;
        WASMComponentResourceType res_type;
        WASMComponentCoreModuleType *core_module_type; /* For DEF_TYPE_KIND_CORE_MODULE */
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

/* Represents a type bound in an export or import description.
 * Spec `typebound`: 0x00 <typeidx> (eq) | 0x01 (sub resource)
 * WAMR's current structure needs review for full alignment.
 * For 'sub resource', the type_idx would implicitly refer to the resource being subtyped,
 * which might require context from the outer type declaration.
 */
typedef enum WASMComponentTypeBoundKind {
    TYPE_BOUND_KIND_EQ = 0x00,           /* Corresponds to spec 0x00 <typeidx> (type equal) */
    TYPE_BOUND_KIND_SUB_RESOURCE = 0x01  /* Corresponds to spec 0x01 (subtype of a resource) */
} WASMComponentTypeBoundKind;

typedef struct WASMComponentTypeBound {
    WASMComponentTypeBoundKind kind;
    /* For TYPE_BOUND_KIND_EQ: `type_idx` is the index of the type it must be equal to.
     * For TYPE_BOUND_KIND_SUB_RESOURCE: The spec `(sub resource)` defines a new abstract resource type
     * that is a subtype of the (implied) resource capabilities of the containing type.
     * The `type_idx` field is not directly used by the `(sub resource)` production itself
     * in the spec. If the parser stores an outer resource type index here for context,
     * the loader must be aware. For a standalone `(sub resource)` defining a fresh type,
     * this field might be unused or set to a sentinel by the parser.
     */
    uint32 type_idx;
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
/* Import/Export Declarations within a Component Type */
typedef struct WASMComponentTypeImportDecl {
    char *name; /* import name */
    WASMComponentExternDesc desc; /* Describes what is being imported */
} WASMComponentTypeImportDecl;

typedef struct WASMComponentTypeExportDecl {
    char *name; /* export name */
    WASMComponentExternDesc desc; /* Describes what is being exported */
} WASMComponentTypeExportDecl;

/* Alias declaration within a Component Type */
/* This describes an alias from an outer scope or to another item within the component type's scope */
typedef struct WASMComponentTypeAliasDecl {
    WASMAliasSort sort; /* func, value, type, component, instance, core module */
    WASMAliasTargetKind target_kind; /* e.g. outer, export (of an instance) */
    uint32 target_outer_depth; /* For outer aliases */
    uint32 target_idx;         /* Index for the target (e.g. instance idx, type idx) */
    char *target_name;         /* Name for export targets */
    char *new_name;            /* Name of the alias being created (if it's for export or lookup) */
                               /* Or this might be implicit by its position in a list of named decls */
} WASMComponentTypeAliasDecl;


/* Declarations within a ComponentType */
typedef enum WASMComponentTypeDeclKind {
    COMPONENT_TYPE_DECL_KIND_IMPORT = 0x00,
    COMPONENT_TYPE_DECL_KIND_EXPORT = 0x01,
    COMPONENT_TYPE_DECL_KIND_ALIAS = 0x02,
    COMPONENT_TYPE_DECL_KIND_CORE_TYPE = 0x03,  /* core:type i t (defines a core type within component type) */
    COMPONENT_TYPE_DECL_KIND_TYPE = 0x04,       /* type i t (defines a component type within component type) */
    /* Future: core instance, instance, etc. */
} WASMComponentTypeDeclKind;

typedef struct WASMComponentTypeDecl {
    WASMComponentTypeDeclKind kind;
    union {
        WASMComponentTypeImportDecl import_decl;
        WASMComponentTypeExportDecl export_decl;
        WASMComponentTypeAliasDecl alias_decl;
        WASMComponentDefinedType type_def; /* For KIND_CORE_TYPE, KIND_TYPE. The def itself. */
                                           /* loader needs to assign an index based on position */
    } u;
    /* Optionally, if these declarations are named for lookup within the component type: */
    /* char *name; */
} WASMComponentTypeDecl;


/* Declarations within an InstanceType */
typedef enum WASMComponentInstanceTypeDeclKind {
    INSTANCE_TYPE_DECL_KIND_EXPORT = 0x00,
    INSTANCE_TYPE_DECL_KIND_ALIAS = 0x01,    /* (alias (instance <idx> <name>) <new_name>) */
    INSTANCE_TYPE_DECL_KIND_CORE_TYPE = 0x02, /* core:type i t */
    INSTANCE_TYPE_DECL_KIND_TYPE = 0x03,      /* type i t */
} WASMComponentInstanceTypeDeclKind;

typedef struct WASMComponentInstanceTypeDecl {
    WASMComponentInstanceTypeDeclKind kind;
    union {
        WASMComponentTypeExportDecl export_decl; /* Exports from the instance type */
        WASMComponentTypeAliasDecl alias_decl;   /* Aliases within the instance type */
        WASMComponentDefinedType type_def;       /* For KIND_CORE_TYPE, KIND_TYPE */
    } u;
    /* Optionally, if these declarations are named: */
    /* char *name; */
} WASMComponentInstanceTypeDecl;


/* ComponentType Definition (as part of WASMComponentDefinedType) */
struct WASMComponentComponentType {
    uint32 decl_count;
    WASMComponentTypeDecl *decls; /* Imports, Exports, Aliases, Type definitions */
};

/* InstanceType Definition (as part of WASMComponentDefinedType) */
struct WASMComponentInstanceType {
    uint32 decl_count;
    WASMComponentInstanceTypeDecl *decls; /* Exports, Aliases, Type definitions */
};

/* Canonical Function Options (Component Model 1.0 Spec opcodes) */
typedef enum WASMComponentCanonicalOptionKind {
    CANONICAL_OPTION_STRING_ENCODING_UTF8       = 0x00, /* string-encoding=utf8 */
    CANONICAL_OPTION_STRING_ENCODING_UTF16      = 0x01, /* string-encoding=utf16 */
    CANONICAL_OPTION_STRING_ENCODING_LATIN1_UTF16 = 0x02, /* string-encoding=latin1+utf16 */
    /* CANONICAL_OPTION_STRING_ENCODING_COMPACT_UTF16 (WAMR specific, 0x03) removed as not in MVP spec */
    CANONICAL_OPTION_MEMORY_IDX                 = 0x03, /* memory <memidx> */
    CANONICAL_OPTION_REALLOC_FUNC_IDX           = 0x04, /* realloc <funcidx> */
    CANONICAL_OPTION_POST_RETURN_FUNC_IDX       = 0x05, /* post-return <funcidx> */
    CANONICAL_OPTION_ASYNC                      = 0x06, /* async (no value) */
    CANONICAL_OPTION_CALLBACK_FUNC_IDX          = 0x07, /* callback <funcidx> */
    CANONICAL_OPTION_ALWAYS_TASK_RETURN         = 0x08  /* always-task-return (no value) */
    /* WAMR's previous CANONICAL_OPTION_ASYNC (0x07), CALLBACK_FUNC_IDX (0x08), ALWAYS_TASK_RETURN (0x09)
       are now mapped to spec 0x06, 0x07, 0x08 respectively.
    */
} WASMComponentCanonicalOptionKind;

typedef struct WASMComponentCanonicalOption {
    WASMComponentCanonicalOptionKind kind;
    uint32 value; /* For idx options, ignored for options without a value */
} WASMComponentCanonicalOption;

/* Structure for a Canonical Section item (Canonical ABI functions)
 * The loader maps Component Model 1.0 spec binary opcodes to these kinds.
 * Spec opcodes:
 *   lift:                      0x00 (func <corefuncidx> (options <canonopt*>) <typeidx>)
 *   lower:                     0x01 (func <funcidx> (options <canonopt*>))
 *   resource.new:              0x02 (resource <typeidx>)
 *   resource.drop:             0x03 (resource <typeidx>)
 *   resource.rep:              0x04 (resource <typeidx>)
 *   (Spec has other canonical functions related to async, tasks, streams, etc.
 *    These will be mapped to WAMR's internal kinds as needed.
 *    The list below is WAMR's current internal set of kinds.)
 */
typedef enum WASMCanonicalFuncKind {
    CANONICAL_FUNC_KIND_LIFT = 0x00,        // Spec: 0x00 lift
    CANONICAL_FUNC_KIND_LOWER = 0x01,       // Spec: 0x01 lower
    CANONICAL_FUNC_KIND_RESOURCE_NEW = 0x02,  // Spec: 0x02 resource.new
    CANONICAL_FUNC_KIND_RESOURCE_DROP = 0x03, // Spec: 0x03 resource.drop
    CANONICAL_FUNC_KIND_RESOURCE_REP = 0x04,  // Spec: 0x04 resource.rep
    /* The following are WAMR-specific extensions or pre-spec versions.
       They will need mapping if they correspond to new spec canonical functions.
       For now, keeping them as internal kinds. The loader must handle this. */
    CANONICAL_FUNC_KIND_TASK_CANCEL = 0x05,
    CANONICAL_FUNC_KIND_SUBTASK_CANCEL = 0x06,
    CANONICAL_FUNC_KIND_RESOURCE_DROP_ASYNC = 0x07,
    CANONICAL_FUNC_KIND_BACKPRESSURE_SET = 0x08,
    CANONICAL_FUNC_KIND_TASK_RETURN = 0x09,
    CANONICAL_FUNC_KIND_CONTEXT_GET = 0x0A,
    CANONICAL_FUNC_KIND_CONTEXT_SET = 0x0B,
    CANONICAL_FUNC_KIND_YIELD = 0x0C,
    CANONICAL_FUNC_KIND_SUBTASK_DROP = 0x0D,
    CANONICAL_FUNC_KIND_STREAM_NEW = 0x0E,
    CANONICAL_FUNC_KIND_STREAM_READ = 0x0F,
    CANONICAL_FUNC_KIND_STREAM_WRITE = 0x10,
    CANONICAL_FUNC_KIND_STREAM_CANCEL_READ = 0x11,
    CANONICAL_FUNC_KIND_STREAM_CANCEL_WRITE = 0x12,
    CANONICAL_FUNC_KIND_STREAM_CLOSE_READABLE = 0x13,
    CANONICAL_FUNC_KIND_STREAM_CLOSE_WRITABLE = 0x14,
    CANONICAL_FUNC_KIND_FUTURE_NEW = 0x15,
    CANONICAL_FUNC_KIND_FUTURE_READ = 0x16,
    CANONICAL_FUNC_KIND_FUTURE_WRITE = 0x17,
    CANONICAL_FUNC_KIND_FUTURE_CANCEL_READ = 0x18,
    CANONICAL_FUNC_KIND_FUTURE_CANCEL_WRITE = 0x19,
    CANONICAL_FUNC_KIND_FUTURE_CLOSE_READABLE = 0x1A,
    CANONICAL_FUNC_KIND_FUTURE_CLOSE_WRITABLE = 0x1B,
    CANONICAL_FUNC_KIND_ERROR_CONTEXT_NEW = 0x1C,
    CANONICAL_FUNC_KIND_ERROR_CONTEXT_DEBUG_MESSAGE = 0x1D,
    CANONICAL_FUNC_KIND_ERROR_CONTEXT_DROP = 0x1E,
    CANONICAL_FUNC_KIND_WAITABLE_SET_NEW = 0x1F,
    CANONICAL_FUNC_KIND_WAITABLE_SET_WAIT = 0x20,
    CANONICAL_FUNC_KIND_WAITABLE_SET_POLL = 0x21,
    CANONICAL_FUNC_KIND_WAITABLE_SET_DROP = 0x22,
    CANONICAL_FUNC_KIND_WAITABLE_JOIN = 0x23,
    CANONICAL_FUNC_KIND_THREAD_SPAWN_REF = 0x40,
    CANONICAL_FUNC_KIND_THREAD_SPAWN_INDIRECT = 0x41,
    CANONICAL_FUNC_KIND_THREAD_AVAILABLE_PARALLELISM = 0x42
} WASMCanonicalFuncKind;

typedef struct WASMComponentCanonical {
    WASMCanonicalFuncKind func_kind;
    uint32 option_count;
    WASMComponentCanonicalOption *options; // Common to many kinds
    union {
        struct { // For LIFT (0x00)
            uint32 core_func_idx;
            uint32 component_func_type_idx;
        } lift;
        struct { // For LOWER (0x01)
            uint32 component_func_idx;
        } lower;
        struct { // For RESOURCE_NEW (0x02), RESOURCE_DROP (0x03), RESOURCE_REP (0x04),
                 // STREAM_NEW (0x0E), STREAM_READ (0x0F), STREAM_WRITE (0x10),
                 // STREAM_CANCEL_READ (0x11), STREAM_CANCEL_WRITE (0x12),
                 // STREAM_CLOSE_READABLE (0x13), STREAM_CLOSE_WRITABLE (0x14),
                 // FUTURE_NEW (0x15), ... FUTURE_CLOSE_WRITABLE (0x1B),
                 // THREAD_SPAWN_REF (0x40), RESOURCE_DROP_ASYNC (0x07)
            uint32 type_idx; // Represents rt (resource type index) or general type_idx
        } type_idx_op;
        struct { // For CONTEXT_GET (0x0A), CONTEXT_SET (0x0B)
            uint32 context_op_idx;
        } context_op;
        struct { // For WAITABLE_SET_WAIT (0x20), WAITABLE_SET_POLL (0x21)
            uint8 async_opt; // 0x00 or 0x01, (directly parsed, not an option)
            uint32 mem_idx;
        } waitable_mem_op;
        struct { // For THREAD_SPAWN_INDIRECT (0x41)
            uint32 type_idx;
            uint32 table_idx;
        } thread_spawn_indirect_op;
        /* Kinds like TASK_CANCEL (0x05), SUBTASK_CANCEL (0x06), BACKPRESSURE_SET (0x08),
           TASK_RETURN (0x09), YIELD (0x0C), SUBTASK_DROP (0x0D), ERROR_CONTEXT_NEW (0x1C),
           ERROR_CONTEXT_DEBUG_MESSAGE (0x1D), ERROR_CONTEXT_DROP (0x1E),
           WAITABLE_SET_NEW (0x1F), WAITABLE_SET_DROP (0x22), WAITABLE_JOIN (0x23),
           THREAD_AVAILABLE_PARALLELISM (0x42)
           may only use options or have no specific fields before options.
           For example, YIELD has an 'async?' byte that might be parsed before options.
           However, the new CANONICAL_OPTION_ASYNC suggests 'async' is an option.
           For this structure, direct fields are preferred if they are not part of the 'opts' vector.
           Revisit specific parsing based on Binary.md details for each.
           For now, this union covers explicitly field-taking kinds.
        */
    } u;
} WASMComponentCanonical;

/* Structure for a Start Section item (Component Start Function) */
typedef struct WASMComponentStart {
    uint32 func_idx; /* index to a component function (defined via canonical lift or alias) */
    uint32 arg_count;
    uint32 *arg_value_indices; /* Indices to value items in the component's context */
    /* TODO: Define structures for actual values if they are inlined rather than indexed */
} WASMComponentStart;

/* Structure for an Import Section item (Component Imports) */
typedef struct WASMComponentImport {
    char *name; /* import name (module name for linking) */
    WASMComponentExternDesc desc; /* Describes what is being imported (e.g. a func with type_idx, a component with type_idx) */
} WASMComponentImport;

/* Kinds of items that can be exported from a component */
typedef enum WASMComponentExportKind {
    EXPORT_KIND_FUNC = 0x00,
    EXPORT_KIND_VALUE = 0x01,
    EXPORT_KIND_TYPE = 0x02,
    EXPORT_KIND_COMPONENT = 0x03,
    EXPORT_KIND_INSTANCE = 0x04,
    /* EXPORT_KIND_MODULE (core module) is not listed for component exports, but for core instance exports */
} WASMComponentExportKind;

/* Structure for an Export Section item (Component Exports) */
typedef struct WASMComponentExport {
    char *name; /* export name */
    WASMComponentExportKind kind; /* func, value, type, component, instance */
    uint32 item_idx; /* index to the item in the component's appropriate index space */
    uint32 optional_desc_type_idx; /* Optional: (type <typeidx>) type annotation. (uint32)-1 if not present.
                                      This type_idx points to a WASMComponentDefinedType. */
} WASMComponentExport;


/* Main structure representing a parsed WASM Component */
typedef struct WASMComponent {
    uint32 version;
    uint32 layer; /* Should be 1 for current spec */

    /* Arrays of component sections */
    WASMComponentCoreModule *core_modules;          /* Section ID 0 */
    uint32 core_module_count;

    WASMComponentCoreInstance *core_instances;      /* Section ID 1 */
    uint32 core_instance_count;

    WASMComponentCoreTypeDef *core_type_defs;       /* Section ID 2: core:type definitions */
    uint32 core_type_def_count;                     /* (e.g. core func types, core module types) */

    WASMComponentNestedComponent *nested_components;/* Section ID 3: nested component definitions */
    uint32 nested_component_count;

    WASMComponentInstance *component_instances;     /* Section ID 4: component instances */
    uint32 component_instance_count;

    WASMComponentAlias *aliases;                    /* Section ID 6 (Spec): aliases */
    uint32 alias_count;

    /* Section ID 7 (Spec): Unified type definitions for all component-level types
     * (functype, componenttype, instancetype, resourcetype, valtype, etc.).
     * This single list now holds all types previously split between
     * WAMR's old 'type_definitions' (section 6) and 'component_type_definitions' (section 7).
     * The kind field within WASMComponentDefinedType distinguishes the specific type.
     */
    WASMComponentDefinedType *type_definitions;     /* All component-level type definitions */
    uint32 type_definition_count;                   /* Count for all type_definitions */

    WASMComponentCanonical *canonicals;             /* Section ID 8 (Spec): canonical functions */
    uint32 canonical_count;

    WASMComponentStart *starts;                     /* Section ID 9 (Spec): start functions */
    uint32 start_count;

    WASMComponentImport *imports;                   /* Section ID 10: imports */
    uint32 import_count;

    WASMComponentExport *exports;                   /* Section ID 11: exports */
    uint32 export_count;

    /* Custom sections are not explicitly listed here but would be handled by the loader */
    /* Memory management for the component and its structures is handled by the loader */

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
