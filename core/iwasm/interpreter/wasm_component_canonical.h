/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_COMPONENT_CANONICAL_H
#define _WASM_COMPONENT_CANONICAL_H

#include "wasm_component_loader.h" /* For WASMComponentCanonical, WASMComponentValType */
#include "wasm_exec_env.h"         /* For WASMExecEnv */
#include "../common/wasm_c_api_internal.h" /* For bool, uint32, etc. */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lifts a core WebAssembly value to a component model value.
 *
 * @param exec_env the execution environment of the calling core Wasm module.
 *                 This is used to access memory and call realloc if needed.
 * @param canonical_def definition of the canonical function being processed,
 *                      containing options like memory index and realloc function index.
 * @param core_func_idx index of the core function (e.g., the function whose
 *                      result is being lifted), potentially used for context or error reporting.
 * @param core_value_ptr pointer to the core Wasm value(s). For simple primitives,
 *                       this might be a direct pointer to the value on the stack or
 *                       in an arguments array. For strings/lists, this could be
 *                       an offset/length pair in core Wasm memory.
 * @param core_value_type the type tag of the core Wasm value (e.g., VALUE_TYPE_I32).
 *                        This might be redundant if `target_component_valtype` is sufficient.
 * @param target_component_valtype the target component model value type.
 * @param lifted_component_value_ptr output parameter; on success, this will point
 *                                   to the newly allocated/created component model value.
 *                                   The caller is responsible for managing this memory
 *                                   (e.g., freeing it after use, potentially via lowering).
 * @param error_buf buffer to write error messages to.
 * @param error_buf_size size of the error buffer.
 * @return true on success, false on failure (with error_buf populated).
 */
bool
wasm_component_canon_lift_value(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def,
    uint32 core_func_idx, /* For context or if options are per-function */
    void *core_value_ptr, /* e.g. uint32* for i32, or a struct for (offset, len) */
    uint8 core_value_type,  /* e.g. VALUE_TYPE_I32 from wasm.h */
    const WASMComponentValType *target_component_valtype,
    void **lifted_component_value_ptr,
    char *error_buf, uint32 error_buf_size);

/**
 * Lowers a component model value to a core WebAssembly value.
 *
 * @param exec_env the execution environment of the calling core Wasm module.
 *                 This is used to access memory and call malloc if needed.
 * @param canonical_def definition of the canonical function being processed,
 *                      containing options like memory index and realloc/alloc function index.
 * @param core_func_idx index of the core function (e.g., the function being called
 *                      with the lowered arguments), potentially used for context.
 * @param component_value_ptr pointer to the component model value.
 * @param source_component_valtype the type of the source component model value.
 * @param target_core_wasm_type the target core Wasm type tag (e.g., VALUE_TYPE_I32).
 *                              For strings, this might indicate an i32 pair (offset, length) is expected.
 * @param core_value_write_ptr output parameter; pointer to where the core Wasm value(s)
 *                             should be written. For primitives, this is a direct pointer.
 *                             For strings, this would receive the (offset, length) pair.
 * @param error_buf buffer to write error messages to.
 * @param error_buf_size size of the error buffer.
 * @return true on success, false on failure (with error_buf populated).
 */
bool
wasm_component_canon_lower_value(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def,
    uint32 core_func_idx,
    void *component_value_ptr,
    const WASMComponentValType *source_component_valtype,
    uint8 target_core_wasm_type, /* e.g. VALUE_TYPE_I32 or VALUE_TYPE_I64 for string pair */
    void *core_value_write_ptr,  /* e.g. uint32* for primitives, uint32[2]* for string pair */
    char *error_buf, uint32 error_buf_size);


/* Host-side representation for a resource. */
/* This is a simplified internal tracking structure for WAMR. */
typedef struct WAMRHostResourceEntry {
    bool is_active;                     /* Whether this handle is currently active */
    uint32_t component_resource_type_idx; /* Type index of the resource within the component's type definitions */
    void *host_data;                    /* Pointer to actual host-specific data for the resource (if any) */
                                        /* For now, this will be NULL. Actual resource methods would manage this. */
    // --- Fields for Destructor Support ---
    WASMModuleInstance *owner_module_inst;  /* Module instance that owns the destructor */
    uint32 dtor_core_func_idx;          /* Core function index of the destructor within owner_module_inst. (uint32)-1 if no dtor. */
} WAMRHostResourceEntry;

// Declaration for the global resource table (defined in wasm_component_canonical.c)
// This is needed for wasm_component_runtime.c to access destructor information.
// Ensure MAX_RESOURCE_HANDLES is defined consistently or also exposed.
#define MAX_RESOURCE_HANDLES 128 // Matching the definition in .c file. Consider moving to a shared const.
extern WAMRHostResourceEntry global_resource_table[MAX_RESOURCE_HANDLES];


/**
 * Creates a new resource handle.
 * Part of `canon resource.new typeidx`.
 */
bool
wasm_component_canon_resource_new(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def, /* Contains target resource type_idx in u.type_idx_op.type_idx */
    void *core_value_write_ptr,                  /* Output: int32_t handle for the new resource */
    char *error_buf, uint32 error_buf_size);

/**
 * Drops/destroys a resource.
 * Part of `canon resource.drop typeidx`.
 */
bool
wasm_component_canon_resource_drop(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def, /* Contains target resource type_idx */
    void *component_handle_ptr,                  /* Input: int32_t handle from component */
    char *error_buf, uint32 error_buf_size);

/**
 * Gets the underlying representation of a resource.
 * Part of `canon resource.rep typeidx`.
 */
bool
wasm_component_canon_resource_rep(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def, /* Contains target resource type_idx */
    void *component_handle_ptr,                  /* Input: int32_t handle from component */
    void *core_value_write_ptr,                  /* Output: int32_t representation (usually the handle itself) */
    char *error_buf, uint32 error_buf_size);


#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _WASM_COMPONENT_CANONICAL_H */
