/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_COMPONENT_RUNTIME_H
#define _WASM_COMPONENT_RUNTIME_H

#include "wasm_component_loader.h" /* For WASMComponent */
#include "wasm_runtime.h"          /* For WASMModuleInstance, WASMExecEnv */
#include "../common/wasm_c_api_internal.h" /* For bool, uint32, etc. */

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct WASMComponentInstanceInternal WASMComponentInstanceInternal;
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
        struct {
            void *func_ptr;     // e.g., native C function pointer for a host func, or lifted func
            void *env;          // Environment for this function
            // WASMComponentFuncType *comp_func_type; // Component function type for validation & lifting
        } component_func;

        // If the import is a component model type definition
        // WASMComponentValType *type_def; // Not typically a runtime import item

    } item;
} ResolvedComponentImportItem;


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

    /* TODO: Resolved imports provided to this component instance */
    // void *resolved_imports; 

    /* Execution environment, if one is exclusively owned by this component instance.
       Often, exec_env might be passed in per call to an exported function. */
    // WASMExecEnv *exec_env;

    /* Internal bookkeeping */
    // Map from core_instance_def_idx to index in module_instances array.
    // Only valid for CORE_INSTANCE_KIND_INSTANTIATE. Others can be marked with a sentinel.
    uint32 *core_instance_map; 
};

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
