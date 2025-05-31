/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_COMPONENT_RUNTIME_H
#define _WASM_COMPONENT_RUNTIME_H

#include "wasm_component_loader.h" /* For WASMComponent */
#include "wasm_runtime.h"          /* For WASMModuleInstance, WASMExecEnv */
#include "../common/wasm_c_api_internal.h" /* For bool, uint32, etc. */
#include "bh_thread.h" /* For korp_mutex */

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct WASMComponentInstanceInternal WASMComponentInstanceInternal;
typedef struct WASMComponentFuncJITThunks WASMComponentFuncJITThunks; // New
typedef struct LiftedFuncThunkContext LiftedFuncThunkContext;     // New (or ensure it's declared)
typedef struct WASMModuleInstance WASMModuleInstance;
typedef struct WASMFunctionInstance WASMFunctionInstance;
typedef struct WASMMemoryInstance WASMMemoryInstance;
typedef struct WASMTableInstance WASMTableInstance;
typedef struct WASMGlobalInstance WASMGlobalInstance;
// typedef struct WASMType WASMType; // For component function type, if needed later

// Structure to pass resolved import items to wasm_component_instance_instantiate
typedef struct ResolvedComponentImportItem {
    char *name; // Name of the import as expected by the component/module
    WASMComponentExternKind kind; // The kind of the item being imported
    union {
        // For a core module item being imported (e.g. a function from an outer module)
        WASMFunctionInstance *function;
        WASMMemoryInstance *memory;
        WASMTableInstance *table;
        WASMGlobalInstance *global;
        
        // For a component instance being imported
        WASMComponentInstanceInternal *component_instance;

        // For a function that is defined at the component level (lifted/lowered)
        struct ImportComponentFuncContext { // Renamed for clarity
            void *func_ptr;     // e.g., native C function pointer for a host func
            void *env;          // Environment for this function
            WASMComponentFuncType *func_type; // Component function type for validation & thunk generation
            WASMComponentFuncJITThunks *jit_thunks; /* NULL if not JITted or JIT disabled */
        } import_component_func;

        // If the import is a component model type definition
        // WASMComponentValType *type_def; // Not typically a runtime import item

    } item;
} ResolvedComponentImportItem;

// Structure to represent resolved export items of a component instance
// Definition for JIT thunks associated with a component function
struct WASMComponentFuncJITThunks {
    // For lowering parameters
    void **param_lower_thunks;
    uint8 *param_lower_thunk_exists; // Bitmap or array of bools

    // For lifting results
    void **result_lift_thunks;
    uint8 *result_lift_thunk_exists; // Bitmap or array of bools

    uint32 num_params;
    uint32 num_results;
};

// Context for a component function that might be lifted/lowered, potentially JIT-ted
struct LiftedFuncThunkContext {
    /* Original target function (e.g. core wasm func, or host func for imports) */
    void *original_func_ptr;
    void *original_env; /* Environment for the original_func_ptr */
    WASMComponentFuncType *func_type; /* Component function type */

    /* JIT-compiled thunks for this function */
    WASMComponentFuncJITThunks *jit_thunks; /* NULL if not JITted or JIT disabled */
};

typedef struct ResolvedComponentExportItem {
    char *name; // Name of the export
    uint8 kind; /* ResolvedComponentExportItemKind or WASMComponentExportKind */
    uint32 type_annotation_idx; /* Optional: type index for the export's description */

    union {
        WASMFunctionInstance *function; // Used if kind is core function
        WASMMemoryInstance *memory;
        WASMTableInstance *table;
        WASMGlobalInstance *global;
        WASMComponentInstanceInternal *component_instance;
        LiftedFuncThunkContext *function_thunk_context; // Used for EXPORT_KIND_FUNC
        WASMComponentDefinedType *type_definition; // Used for EXPORT_KIND_TYPE

        // For a value being exported (actual runtime representation of the value)
        void *value_ptr; // Used for EXPORT_KIND_VALUE, type depends on value_type_idx from export_def

    } item;
} ResolvedComponentExportItem;


/**
 * Represents a runtime instance of a WebAssembly Component.
 */
struct WASMComponentInstanceInternal {
    /* Pointer to the component definition */
    WASMComponent *component_def;

    /* Array of instantiated core WebAssembly module instances */
    WASMModuleInstance **module_instances;
    uint32 num_module_instances; // Should match the number of core instances to be created

    /* Array of instantiated nested component instances */
    WASMComponentInstanceInternal **component_instances;
    uint32 num_component_instances; // Should match the number of nested components to instantiate

    /* TODO: Resolved exports of this component instance */
    // void *exports; 
    ResolvedComponentExportItem *resolved_exports;
    uint32 num_resolved_exports;

    /* TODO: Resolved imports provided to this component instance */
    // void *resolved_imports; 
    ResolvedComponentImportItem *resolved_imports;
    uint32 num_resolved_imports;

    /* Execution environment, if one is exclusively owned by this component instance.
       Often, exec_env might be passed in per call to an exported function. */
    WASMExecEnv *exec_env;

    /* Internal bookkeeping */
    // Map from core_instance_def_idx to index in module_instances array.
    // Only valid for CORE_INSTANCE_KIND_INSTANTIATE. Others can be marked with a sentinel.
    uint32 *core_instance_map;

    /* Active resource tracking */
    ActiveResourceHandle *active_resource_list_head;
    korp_mutex active_resource_list_lock;
};

// Definition for ActiveResourceHandle
typedef struct ActiveResourceHandle {
    uint32 resource_type_idx; /* Index into component_def->type_definitions */
    uint32 resource_handle_core_value; /* The actual i32 value representing the handle in core Wasm */
    struct ActiveResourceHandle *next;
} ActiveResourceHandle;

/**
 * Instantiates a WebAssembly Component.
 *
 * @param component The component definition to instantiate.
 * @param parent_exec_env The execution environment of the caller (host or parent component).
 *                        This is used for operations like memory allocation during instantiation
 *                        and potentially for resolving imports if they come from the host.
 * @param import_object If imports are resolved via an import object similar to core Wasm.
 *                      The structure of this needs to be defined for components.
 *                      For now, this might be simplified or handled differently.
 * @param error_buf Buffer to write error messages to.
 * @param error_buf_size Size of the error buffer.
 *
 * @return A pointer to the instantiated component instance, or NULL on failure.
 */
WASMComponentInstanceInternal *
wasm_component_instance_instantiate(
    WASMComponent *component,
    WASMExecEnv *parent_exec_env, /* Used for host interactions during instantiation */
    ResolvedComponentImportItem *resolved_imports,
    uint32 num_resolved_imports,
    char *error_buf,
    uint32 error_buf_size);

/**
 * Deinstantiates a WebAssembly Component instance and frees associated resources.
 *
 * @param comp_inst The component instance to deinstantiate.
 */
void
wasm_component_instance_deinstantiate(WASMComponentInstanceInternal *comp_inst);

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _WASM_COMPONENT_RUNTIME_H */
