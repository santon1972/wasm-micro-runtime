/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _AOT_LLVM_CANONICAL_H_
#define _AOT_LLVM_CANONICAL_H_

#include "aot_compiler.h"
#include "llvm-c/Core.h"

#ifdef __cplusplus
extern "C" {
#endif

LLVMValueRef
aot_compile_lift_string_thunk(AOTCompContext *comp_ctx,
                              AOTFuncContext *func_ctx,
                              uint32_t memory_idx,
                              WASMComponentCanonicalOptionKind string_encoding);

/* Generic helper function to allocate memory on the host, callable from JIT-ted code */
void *
aot_host_alloc_bytes(uint32_t size, void *exec_env_ptr);

/*
 * Compile a thunk for lowering a string from host to Wasm.
 * The JIT-ted thunk signature:
 * void (*thunk_func)(WASMExecEnv *exec_env,
 *                    char *host_str,
 *                    uint32_t host_str_len_bytes,
 *                    uint32_t *out_wasm_offset,
 *                    uint32_t *out_wasm_len_units);
 */
LLVMValueRef
aot_compile_lower_string_thunk(
    AOTCompContext *comp_ctx,
    AOTFuncContext *func_ctx,
    uint32_t memory_idx,
    uint32_t realloc_func_idx,
    WASMComponentCanonicalOptionKind string_encoding);

/* Helper C function for UTF-16 lifting: transcodes from Wasm memory to new host UTF-8 buffer */
char*
aot_transcode_utf16le_to_utf8_on_host_for_lift(WASMExecEnv *exec_env,
                                               uint32_t wasm_mem_idx,
                                               uint32_t str_offset, /* in Wasm memory */
                                               uint32_t str_len_code_units); /* length in UTF-16 code units */

/* Helper C functions to be called from the JIT-ted lower_string_thunk */

/* Calls the Wasm realloc function */
int32_t
aot_call_wasm_realloc(WASMExecEnv *exec_env,
                      uint32_t realloc_fidx,
                      uint32_t old_ptr,
                      uint32_t old_size,
                      uint32_t align,
                      uint32_t new_size);

/* Transcodes UTF-8 to UTF-16LE, allocating buffer on host */
uint16_t *
aot_transcode_utf8_to_utf16le_on_host(const char *utf8_str,
                                      uint32_t utf8_len_bytes,
                                      uint32_t *out_utf16_code_units,
                                      void *exec_env_ptr);

/* Frees a buffer allocated on the host by aot_host_alloc_bytes or similar */
void
aot_host_free_bytes(void *buffer, void *exec_env_ptr);

/* --- Thunks for Primitive Types --- */

/*
 * Compile a thunk for lifting a primitive value from Wasm to host.
 * JIT-ted thunk signature example (for PRIM_VAL_U32):
 *   uint32_t* (*thunk_func)(WASMExecEnv *exec_env, uint32_t core_value);
 */
LLVMValueRef
aot_compile_lift_primitive_thunk(
    AOTCompContext *comp_ctx,
    AOTFuncContext *func_ctx,
    WASMComponentPrimValType primitive_type);

/*
 * Compile a thunk for lowering a primitive value from host to Wasm.
 * JIT-ted thunk signature example (for PRIM_VAL_U32):
 *   void (*thunk_func)(WASMExecEnv *exec_env,
 *                      uint32_t *component_value_ptr,
 *                      uint32_t *out_core_value_ptr);
 */
LLVMValueRef
aot_compile_lower_primitive_thunk(
    AOTCompContext *comp_ctx,
    AOTFuncContext *func_ctx,
    WASMComponentPrimValType primitive_type);

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _AOT_LLVM_CANONICAL_H_ */
