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
#define COMPONENT_SECTION_ID_TYPE           6  /* Defines types available for reference, populates component->type_definitions */
#define COMPONENT_SECTION_ID_DEFINED_TYPE   7  /* Defines component types, func types, value types, etc., populates component->component_type_definitions */
#define COMPONENT_SECTION_ID_CANONICAL      8
#define COMPONENT_SECTION_ID_START          9
#define COMPONENT_SECTION_ID_IMPORT         10
#define COMPONENT_SECTION_ID_EXPORT         11
/* Custom section ID is 0 for core wasm, but component custom sections
   are distinct. The spec does not assign a generic custom section ID for components in the same way.
   The "value" section mentioned in some drafts (e.g. with ID 11) is not in the final binary spec's section list.
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
/* Represents a core:type definition (e.g., core function type, core module type) */
typedef struct WASMComponentCoreTypeDef {
    /* TODO: Define actual structures for core:func, core:module, etc.
       For now, this is a placeholder if we only store raw type data or a tag.
       A full implementation would parse out parameters/results for core func types.
    */
    uint8 kind; /* e.g. 0x60 for func, or a custom kind for core module type */
    /* void *type_definition; // Pointer to a more specific struct */
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
    VAL_TYPE_KIND_OWN_TYPE_IDX,    /* Own a resource, stores typeidx to a resource type definition */
    VAL_TYPE_KIND_BORROW_TYPE_IDX, /* Borrow a resource, stores typeidx to a resource type definition */
    VAL_TYPE_KIND_TYPE_IDX         /* Reference to another defined type (valtype) in a type section */
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
    /* ALIAS_SORT_INSTANCE, ALIAS_SORT_VALUE etc. can be added */
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

/* Represents a component function type */
typedef struct WASMComponentFuncType {
    uint32 param_count;
    WASMComponentLabelValType *params; /* <label, valtype> pairs */
    WASMComponentValType *result;      /* valtype for the result. VOID represented by a specific valtype or NULL */
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
        /* TODO: Add core_module_type if it's to be parsed elaborately */
        /* WASMComponentCoreModuleType core_module_type; */
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
    uint32 type_idx; /* type_idx for both EQ and SUB_RESOURCE (resource type index) */
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

/* Canonical Function Options */
typedef enum WASMComponentCanonicalOptionKind {
    CANONICAL_OPTION_STRING_ENCODING_UTF8,
    CANONICAL_OPTION_STRING_ENCODING_UTF16,
    CANONICAL_OPTION_STRING_ENCODING_COMPACT_UTF16,
    CANONICAL_OPTION_MEMORY_IDX,    /* value is core memory index */
    CANONICAL_OPTION_REALLOC_FUNC_IDX, /* value is core func index for realloc */
    CANONICAL_OPTION_POST_RETURN_FUNC_IDX /* value is core func index for post-return */
} WASMComponentCanonicalOptionKind;

typedef struct WASMComponentCanonicalOption {
    WASMComponentCanonicalOptionKind kind;
    uint32 value; /* For idx options */
} WASMComponentCanonicalOption;

/* Structure for a Canonical Section item (Canonical ABI functions) */
typedef enum WASMCanonicalFuncKind {
    CANONICAL_FUNC_KIND_LIFT = 0x00,
    CANONICAL_FUNC_KIND_LOWER = 0x01
} WASMCanonicalFuncKind;

typedef struct WASMComponentCanonical {
    WASMCanonicalFuncKind func_kind; /* lift or lower */
    uint32 core_func_idx;      /* Index to the core function */
    uint32 component_func_type_idx; /* Index to a component function type in a type section */
    uint32 option_count;
    WASMComponentCanonicalOption *options;
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

    WASMComponentAlias *aliases;                    /* Section ID 5: aliases */
    uint32 alias_count;

    WASMComponentDefinedType *type_definitions;     /* Section ID 6: type definitions (functype, componenttype, etc.) */
    uint32 type_definition_count;

    WASMComponentDefinedType *component_type_definitions; /* Section ID 7: component type definitions */
    uint32 component_type_definition_count;         /* (Structure is same as type_definitions, but from a different section for component-specific types) */

    WASMComponentCanonical *canonicals;             /* Section ID 8: canonical functions */
    uint32 canonical_count;

    WASMComponentStart *starts;                     /* Section ID 9: start functions */
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
