/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _AOT_COMPONENT_CANONICAL_H_
#define _AOT_COMPONENT_CANONICAL_H_

#include "aot_compiler.h" /* Includes aot_component_types.h */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lifts a core WebAssembly value to its canonical representation.
 *
 * This function converts a value from its representation in core WebAssembly
 * (e.g., an i32, a pair of i32s for a string/list) into the
 * Component Model Canonical ABI representation.
 *
 * @param comp_ctx The AOT compilation context.
 * @param func_ctx The AOT function compilation context.
 * @param core_val_ptr A pointer to an array of LLVMValueRef representing the
 *                     core WebAssembly value(s). For a single primitive,
 *                     this will be a pointer to one LLVMValueRef. For a string
 *                     or list, it might be a pointer to two LLVMValueRefs
 *                     (e.g., offset and length). The function will consume
 *                     the necessary values based on core_wasm_type.
 * @param core_wasm_type An array of WASMType indicating the type(s) of the
 *                       core WebAssembly value(s) pointed to by core_val_ptr.
 *                       The length of this array should correspond to the
 *                       number of core values expected for the target_canon_type.
 * @param num_core_vals The number of elements in core_val_ptr and core_wasm_type.
 * @param target_canon_type A pointer to the AOTCanonValType describing the
 *                          target canonical type to which the core value
 *                          should be lifted.
 * @param error_flag An output parameter (LLVMValueRef to an i1). This will be
 *                   set to true (1) by the function if an error occurs during
 *                   lifting (e.g., memory allocation failure, type mismatch,
 *                   unsupported type). Otherwise, it will be set to false (0).
 *                   The caller must initialize this to false.
 *
 * @return An LLVMValueRef representing the lifted value in its canonical form.
 *         The exact nature of this LLVMValueRef depends on the target_canon_type.
 *         For example, for a canonical primitive, it's the LLVM value itself.
 *         For a list or record, it might be a pointer to the structure in
 *         canonical memory. Returns NULL or a poison value if an error occurs,
 *         in which case error_flag will be set.
 */
LLVMValueRef
aot_canon_lift_value(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
                     LLVMValueRef *core_val_ptr, WASMType *core_wasm_type,
                     uint32_t num_core_vals,
                     const AOTCanonValType *target_canon_type,
                     LLVMValueRef *error_flag);

/**
 * @brief Lowers a canonical value to its core WebAssembly representation.
 *
 * This function converts a value from its representation in the Component Model
 * Canonical ABI into one or more core WebAssembly values (e.g., an i32,
 * a pair of i32s for a string/list).
 *
 * @param comp_ctx The AOT compilation context.
 * @param func_ctx The AOT function compilation context.
 * @param canon_val An LLVMValueRef representing the canonical value. The exact
 *                  nature of this LLVMValueRef depends on the source_canon_type.
 * @param source_canon_type A pointer to the AOTCanonValType describing the
 *                          source canonical type of canon_val.
 * @param target_core_wasm_type An array indicating the target core WebAssembly
 *                              type(s). The function will produce values of these
 *                              types. The caller must ensure this array is large
 *                              enough to hold the expected number of return values
 *                              (e.g., 2 for a string: offset, length).
 * @param num_target_core_vals A pointer to a uint32_t that will be filled by the
 *                             function with the number of core WebAssembly values
 *                             written to target_core_wasm_type_ptr and returned
 *                             via the function's multi-return mechanism (if applicable)
 *                             or as a struct pointer.
 * @param error_flag An output parameter (LLVMValueRef to an i1). This will be
 *                   set to true (1) by the function if an error occurs during
 *                   lowering. Otherwise, it will be set to false (0).
 *                   The caller must initialize this to false.
 *
 * @return An LLVMValueRef. If num_target_core_vals is 1, this is the single
 *         lowered core WebAssembly value. If num_target_core_vals > 1, this
 *         might be a pointer to a struct containing the multiple values, or
 *         the function might use LLVM's multi-return capabilities directly
 *         (this design detail might evolve). Returns NULL or a poison value
 *         if an error occurs, in which case error_flag will be set.
 *         For now, we will assume for types like string/list that return 2 values,
 *         it will return a pointer to a struct {i32, i32}.
 *         For primitives, it returns the direct value.
 */
LLVMValueRef
aot_canon_lower_value(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
                      LLVMValueRef canon_val,
                      const AOTCanonValType *source_canon_type,
                      WASMType *target_core_wasm_types, /* out: array to be filled */
                      uint32_t *num_target_core_vals,   /* out: number of values filled */
                      LLVMValueRef *error_flag);

/**
 * @brief Gets an existing or creates a new AOT wrapper function for a cross-component call.
 *
 * This function is responsible for generating a specific LLVM IR function (wrapper)
 * that handles the translation between core WebAssembly ABI and Component Model
 * Canonical ABI for a given imported function that resides in another component.
 *
 * The generated wrapper will:
 * 1. Take arguments in core WebAssembly ABI (as LLVMValueRefs).
 * 2. Lower these arguments to their Canonical ABI representations using aot_canon_lower_value.
 * 3. Call the actual target component function (which expects Canonical ABI arguments).
 *    (The mechanism for this call is simplified in initial implementations).
 * 4. Lift the Canonical ABI return value from the target function back to core Wasm ABI
 *    using aot_canon_lift_value.
 * 5. Return the lifted core Wasm value.
 *
 * @param comp_ctx The AOT compilation context.
 * @param func_ctx The AOT function context of the function *containing* the call op.
 *                 This is used to add the wrapper to the correct LLVM module.
 * @param import_func_idx The index of the imported function in `comp_ctx->comp_data->import_funcs`.
 *                        This function must be marked with `is_cross_component_call = true`.
 *
 * @return LLVMValueRef representing the LLVM function of the wrapper.
 *         Returns NULL if an error occurs (e.g., import_func_idx is invalid,
 *         the import is not a cross-component call, or memory allocation failure).
 *         Error details will be set via aot_set_last_error.
 */
LLVMValueRef
aot_get_or_create_component_call_wrapper(AOTCompContext *comp_ctx,
                                         AOTFuncContext *func_ctx,
                                         uint32 import_func_idx);

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _AOT_COMPONENT_CANONICAL_H_ */
