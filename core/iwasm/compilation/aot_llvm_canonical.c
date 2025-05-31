/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "aot_llvm_canonical.h"
#include "aot_comp_context.h"
#include "aot_function.h"
#include "aot_emit_memory.h"    /* For memory access checks and base address */
#include "aot_emit_exception.h" /* For exception handling, if needed later */
#include "aot_emit_function.h"  /* For aot_get_func_type_by_idx */
#include "llvm-c/Core.h"
#include "llvm-c/Analysis.h" /* For LLVMVerifyFunction */
#include "wasm_component_canonical.h" /* For transcode_utf8_to_utf16le_on_host */
#include "../../interpreter/wasm_runtime.h" /* for wasm_runtime_call_wasm */
#include "llvm-c/Transforms/Scalar.h" /* For LLVMAddPromoteMemoryToRegisterPass */
#include "../aot_runtime.h" /* For AOTModuleInstance, WASMExecEnv */
#include "../../common/wasm_runtime_common.h" /* For loader_malloc */

/* External helper function implementation */
/* Renamed from aot_host_malloc_for_lifted_string */
void *
aot_host_alloc_bytes(uint32_t size, void *exec_env_ptr)
{
    WASMExecEnv *exec_env = (WASMExecEnv *)exec_env_ptr;
    /* TODO: Proper error handling using exec_env if loader_malloc fails.
     * For now, just returning NULL is fine for the PoC.
     * loader_malloc might not be the final function,
     * could be bh_malloc or similar depending on WAMR's internal allocators.
     */
    (void)exec_env; /* exec_env might be used later for setting exceptions */
    return loader_malloc(size, NULL, 0);
}

LLVMValueRef
aot_compile_lift_utf8_string_thunk(AOTCompContext *comp_ctx,
                                   AOTFuncContext *func_ctx,
                                   uint32_t memory_idx)
{
    LLVMContextRef context = comp_ctx->context;
    LLVMModuleRef module = comp_ctx->module;
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMValueRef func = NULL;
    LLVMTypeRef func_type_ref, param_types[3], ret_type;
    LLVMBasicBlockRef entry_block, then_block, else_block, merge_block;
    LLVMBasicBlockRef oob_check_block, oob_ret_null_block, post_oob_block;
    LLVMBasicBlockRef malloc_fail_block, malloc_succ_block;
    LLVMValueRef exec_env_ptr, str_offset, str_len_bytes;
    LLVMValueRef module_inst_ptr, mem_base_ptr; /* Removed mem_base_ptr_ptr */
    LLVMValueRef mem_data_size; /* Removed mem_data_size_ptr */
    LLVMValueRef end_offset, cmp_oob;
    LLVMValueRef host_buffer_ptr;
    LLVMValueRef total_alloc_size;
    LLVMValueRef wasm_src_addr;
    LLVMValueRef null_terminator_addr, null_char;
    char func_name[64];

    bh_assert(memory_idx == 0 && "Currently only memory index 0 is supported for lift thunks");

    /* Define function type: char* thunk(WASMExecEnv *exec_env, uint32_t str_offset, uint32_t str_len_bytes) */
    ret_type = LLVMPointerType(LLVMInt8TypeInContext(context), 0); /* char* */
    param_types[0] = LLVMPointerType(comp_ctx->exec_env_type, 0);  /* WASMExecEnv* */
    param_types[1] = LLVMInt32TypeInContext(context);              /* str_offset */
    param_types[2] = LLVMInt32TypeInContext(context);              /* str_len_bytes */

    func_type_ref = LLVMFunctionType(ret_type, param_types, 3, false);

    /* Create function */
    snprintf(func_name, sizeof(func_name), "aot_lift_utf8_string_mem%u", memory_idx);
    if (!(func = LLVMAddFunction(module, func_name, func_type_ref))) {
        aot_set_last_error("LLVMAddFunction failed for lift_utf8_string thunk.");
        return NULL;
    }
    LLVMSetFunctionCallConv(func, LLVMCCallConv); /* Standard C calling convention */

    /* Get function parameters */
    exec_env_ptr = LLVMGetParam(func, 0);
    LLVMSetValueName(exec_env_ptr, "exec_env");
    str_offset = LLVMGetParam(func, 1);
    LLVMSetValueName(str_offset, "str_offset");
    str_len_bytes = LLVMGetParam(func, 2);
    LLVMSetValueName(str_len_bytes, "str_len_bytes");

    /* Create entry basic block */
    entry_block = LLVMAppendBasicBlockInContext(context, func, "entry");
    LLVMPositionBuilderAtEnd(builder, entry_block);

    /* Get WASMModuleInstance */
    /* module_inst = exec_env->module_inst */
    module_inst_ptr = LLVMBuildLoad2(builder, comp_ctx->exec_env_type, /* This should be module_inst_type */
                                     LLVMBuildStructGEP2(builder, comp_ctx->exec_env_type, exec_env_ptr,
                                                         AOT_EXEC_ENV_MODULE_INST_OFFISET, "module_inst_addr"),
                                     "module_inst");

    /* Get Memory Base and Size from AOTMemInfo (populated during memory declaration) */
    AOTMemInfo *mem_info = &comp_ctx->memories[memory_idx];

    /* Load memory base address from the global variable pointer in AOTMemInfo */
    /* mem_info->mem_base_addr_val is i8** (LLVMValueRef for the global variable) */
    mem_base_ptr = LLVMBuildLoad2(builder,
                                  LLVMPointerType(LLVMInt8TypeInContext(context), 0), /* Expected type: i8* */
                                  mem_info->mem_base_addr_val,
                                  "mem_base_addr");

    /* Load current memory size in bytes.
       mem_info->cur_page_count_val is i32* (LLVMValueRef for the global variable storing page count) */
    LLVMValueRef page_size_const = LLVMConstInt(LLVMInt32TypeInContext(context), WASM_PAGE_SIZE, false);
    LLVMValueRef cur_page_count_ptr = mem_info->cur_page_count_val;
    LLVMValueRef cur_page_count = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), cur_page_count_ptr, "cur_page_count");
    mem_data_size = LLVMBuildMul(builder, cur_page_count, page_size_const, "mem_data_size_i32");
    /* Extend to i64 for bounds checking, as str_offset + str_len_bytes could exceed i32 max if not careful,
       though inputs are i32. Using i64 for the check is safer. */
    mem_data_size = LLVMBuildZExt(builder, mem_data_size, LLVMInt64TypeInContext(context), "mem_data_size_i64");

    /* Bounds Checking: str_offset + str_len_bytes > mem_data_size */
    /* Note: str_offset and str_len_bytes are i32. mem_data_size is i64. */
    /* Need to extend str_offset and str_len_bytes to i64 for the sum, or mem_data_size to i32 if sum cannot overflow i32 */
    LLVMValueRef str_offset_i64 = LLVMBuildZExt(builder, str_offset, LLVMInt64TypeInContext(context), "str_offset_i64");
    LLVMValueRef str_len_bytes_i64 = LLVMBuildZExt(builder, str_len_bytes, LLVMInt64TypeInContext(context), "str_len_bytes_i64");
    end_offset = LLVMBuildAdd(builder, str_offset_i64, str_len_bytes_i64, "end_offset");

    /* If str_len_bytes is 0, end_offset can be equal to str_offset.
       The check should be end_offset > mem_data_size.
       If str_len_bytes is 0, it means an empty string.
       If str_offset == mem_data_size and str_len_bytes == 0, it's valid (pointer to end, 0 length).
       If str_offset > mem_data_size, it's an OOB access.
       So, check str_offset < mem_data_size first for non-empty strings,
       or (str_offset <= mem_data_size && str_offset + str_len_bytes <= mem_data_size)
       The current `end_offset > mem_data_size` correctly covers these:
       e.g. offset=10, len=5, size=12 -> end=15. 15 > 12 -> OOB. Correct.
       e.g. offset=10, len=0, size=12 -> end=10. 10 > 12 -> False. Correct.
       e.g. offset=12, len=0, size=12 -> end=12. 12 > 12 -> False. Correct.
       e.g. offset=13, len=0, size=12 -> end=13. 13 > 12 -> OOB. Correct.
    */
    cmp_oob = LLVMBuildICmp(builder, LLVMIntUGT, end_offset, mem_data_size, "cmp_oob");

    /* It might also be necessary to check if str_offset itself is already OOB,
       i.e. str_offset >= mem_data_size if str_len_bytes > 0.
       Or str_offset > mem_data_size if str_len_bytes == 0.
       The current check `str_offset_i64 + str_len_bytes_i64 > mem_data_size` handles this.
       If str_offset >= mem_data_size, and len > 0, then sum will be > mem_data_size.
       If str_offset > mem_data_size, and len = 0, then sum will be > mem_data_size.
       If str_offset = mem_data_size, and len = 0, sum equals mem_data_size, not >. This is valid for 0-length.
    */

    oob_ret_null_block = LLVMAppendBasicBlockInContext(context, func, "oob_ret_null");
    post_oob_block = LLVMAppendBasicBlockInContext(context, func, "post_oob_check");
    /* Note: Removed oob_check_block as it was just a passthrough */
    LLVMBuildCondBr(builder, cmp_oob, oob_ret_null_block, post_oob_block);

    LLVMPositionBuilderAtEnd(builder, oob_ret_null_block);
    LLVMBuildRet(builder, LLVMConstNull(ret_type)); /* Return NULL if OOB */

    LLVMPositionBuilderAtEnd(builder, post_oob_block);

    /* Allocate Host Memory: total_alloc_size = str_len_bytes + 1 (for null terminator) */
    /* Check for potential overflow if str_len_bytes is near max_i32 */
    LLVMValueRef one_i32 = LLVMConstInt(LLVMInt32TypeInContext(context), 1, false);
    total_alloc_size = LLVMBuildNSWAdd(builder, str_len_bytes, one_i32, "total_alloc_size");
    // TODO: Add check if total_alloc_size wrapped around (became < str_len_bytes).
    // For PoC, assume str_len_bytes is reasonable and won't overflow when adding 1.

    /* Declare and call aot_host_malloc_for_lifted_string */
    LLVMTypeRef malloc_param_types[] = { LLVMInt32TypeInContext(context), LLVMPointerType(comp_ctx->exec_env_type, 0) }; /* exec_env_type needs to be void* or actual type */
    LLVMTypeRef malloc_func_type = LLVMFunctionType(ret_type /* char* */, malloc_param_types, 2, false);

    /* Get or declare the aot_host_malloc_for_lifted_string function */
    LLVMValueRef malloc_func_ptr = LLVMGetNamedFunction(module, "aot_host_alloc_bytes");
    if (!malloc_func_ptr) {
        malloc_func_ptr = LLVMAddFunction(module, "aot_host_alloc_bytes", malloc_func_type);
        LLVMSetFunctionCallConv(malloc_func_ptr, LLVMCCallConv); /* Ensure C calling convention */
    }

    /* Cast exec_env_ptr to void* if malloc helper expects void* for exec_env */
    /* The C function aot_host_alloc_bytes takes void*, so cast exec_env_ptr (WASMExecEnv*) to void* */
    LLVMValueRef exec_env_void_ptr = LLVMBuildBitCast(builder, exec_env_ptr, LLVMPointerType(LLVMInt8TypeInContext(context),0), "exec_env_void_ptr");

    LLVMValueRef malloc_args[] = { total_alloc_size, exec_env_void_ptr };
    host_buffer_ptr = LLVMBuildCall2(builder, malloc_func_type, malloc_func_ptr, malloc_args, 2, "host_buffer_raw");
    /* host_buffer_ptr is already char* (i8*), no further cast needed from the call itself */

    /* Check if host_buffer_ptr is NULL */
    LLVMValueRef cmp_malloc_fail = LLVMBuildICmp(builder, LLVMIntEQ, host_buffer_ptr, LLVMConstNull(ret_type), "cmp_malloc_fail");
    malloc_fail_block = LLVMAppendBasicBlockInContext(context, func, "malloc_fail_ret_null");
    malloc_succ_block = LLVMAppendBasicBlockInContext(context, func, "malloc_success");

    LLVMBuildCondBr(builder, cmp_malloc_fail, malloc_fail_block, malloc_succ_block);

    LLVMPositionBuilderAtEnd(builder, malloc_fail_block);
    LLVMBuildRet(builder, LLVMConstNull(ret_type)); /* Return NULL if malloc failed */

    LLVMPositionBuilderAtEnd(builder, malloc_succ_block);

    /* Copy String Data */
    /* wasm_src_addr = mem_base_ptr + str_offset (extend str_offset to pointer size if necessary) */
    LLVMValueRef str_offset_ptrsize;
    if (comp_ctx->pointer_size == sizeof(uint64_t)) { /* 64-bit target */
        str_offset_ptrsize = LLVMBuildZExt(builder, str_offset, LLVMInt64TypeInContext(context), "str_offset_ptrsize");
    } else { /* 32-bit target */
        str_offset_ptrsize = str_offset;
    }
    wasm_src_addr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(context), mem_base_ptr, &str_offset_ptrsize, 1, "wasm_src_addr");

    /* Call llvm.memcpy.p0i8.p0i8.i32 (or i64 for length, based on pointer size) */
    LLVMTypeRef memcpy_len_type = (comp_ctx->pointer_size == sizeof(uint64_t))
                                    ? LLVMInt64TypeInContext(context)
                                    : LLVMInt32TypeInContext(context);
    LLVMValueRef str_len_for_memcpy;
    if (comp_ctx->pointer_size == sizeof(uint64_t)) {
        str_len_for_memcpy = LLVMBuildZExt(builder, str_len_bytes, LLVMInt64TypeInContext(context), "str_len_memcpy64");
    } else {
        str_len_for_memcpy = str_len_bytes;
    }

    /* Build call to llvm.memcpy intrinsic */
    LLVMValueRef memcpy_args[] = {
        host_buffer_ptr, /* dest */
        wasm_src_addr,   /* src */
        str_len_for_memcpy, /* len */
        LLVMConstInt(LLVMInt1TypeInContext(context), 0, false) /* is_volatile = false */
    };

    LLVMTypeRef memcpy_arg_types[] = {
        LLVMPointerType(LLVMInt8TypeInContext(context),0), /* PtrToInt8 dest */
        LLVMPointerType(LLVMInt8TypeInContext(context),0), /* PtrToInt8 src */
        memcpy_len_type /* length type */
    };
    LLVMValueRef memcpy_func = LLVMGetIntrinsicDeclaration(module, "llvm.memcpy", memcpy_arg_types, 3);
    /* Note: LLVMIntrinsicGetType is not needed when calling intrinsics obtained via LLVMGetIntrinsicDeclaration */
    LLVMBuildCall2(builder, LLVMGetCalledFunctionType(memcpy_func), memcpy_func, memcpy_args, 4, "");


    /* Null Terminate: host_buffer[str_len_bytes] = '\0' */
    /* GEP expects index to be of pointer size for array indexing if the base pointer is just i8* and not an array type */
    /* However, since host_buffer_ptr is i8*, the index for GEP should match the size of str_len_bytes or be extended.
       LLVMBuildGEP2 with a single index on an i8* effectively does pointer arithmetic: ptr + index.
       The type of the index for GEP2 on a pointer (not struct) should match the pointer size type if we want byte addressing,
       or it can be any integer type and LLVM handles it.
       For safety and clarity, using the pointer-size integer type for the index is common.
    */
    LLVMValueRef null_idx;
    if (comp_ctx->pointer_size == sizeof(uint64_t)) {
        null_idx = LLVMBuildZExt(builder, str_len_bytes, LLVMInt64TypeInContext(context), "null_idx_i64");
    }
    else {
        null_idx = str_len_bytes; /* Already i32 */
    }
    null_terminator_addr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(context), host_buffer_ptr, &null_idx, 1, "null_term_addr");
    null_char = LLVMConstInt(LLVMInt8TypeInContext(context), 0, false); /* i8 0 */
    LLVMBuildStore(builder, null_char, null_terminator_addr);

    /* Return host_buffer_ptr (which is i8*) */
    LLVMBuildRet(builder, host_buffer_ptr);

    /* Basic verification and optimization passes (optional but good practice) */
    if (LLVMVerifyFunction(func, LLVMPrintMessageAction) != 0) {
        fprintf(stderr, "LLVMVerifyFunction failed for %s\n", func_name);
        // LLVMDumpValue(func); // Dump IR for debugging
        // Consider removing the function if it's invalid to avoid LLVM errors later
        // LLVMDeleteFunction(func);
        // return NULL;
    }
    // Example of running passes, func_ctx might be needed for pass manager
    // if (func_ctx && func_ctx->pass_mgr) {
    //     LLVMRunFunctionPassManager(func_ctx->pass_mgr, func);
    // } else {
    //     // Or run some basic passes directly if no func_ctx pass manager
    //     LLVMPassManagerRef basic_pass_mgr = LLVMCreatePassManager();
    //     LLVMAddPromoteMemoryToRegisterPass(basic_pass_mgr); // Mem2Reg
    //     LLVMAddInstructionCombiningPass(basic_pass_mgr);    // Combine redundant instructions
    //     LLVMAddCFGSimplificationPass(basic_pass_mgr);      // Simplify control flow
    //     LLVMRunPassManager(basic_pass_mgr, module); // This runs on the whole module, better to run on function
    //     LLVMDisposePassManager(basic_pass_mgr);
    // }
    return func;
}

/* Helper C function implementations for lowering */

int32_t
aot_call_wasm_realloc(WASMExecEnv *exec_env,
                      uint32_t realloc_fidx,
                      uint32_t old_ptr,
                      uint32_t old_size,
                      uint32_t align,
                      uint32_t new_size)
{
    AOTModuleInstance *module_inst =
        (AOTModuleInstance *)wasm_runtime_get_module_inst(exec_env);
    AOTFunctionInstance *realloc_func;
    WASMType *realloc_func_type;
    uint32 argv[4];
    uint32 argc = 4;

    bh_assert(module_inst);
    bh_assert(realloc_fidx < module_inst->module->import_function_count
                                 + module_inst->module->function_count);

    realloc_func = module_inst->func_insts[realloc_fidx];
    bh_assert(realloc_func);

    realloc_func_type = realloc_func->func_type;
    bh_assert(realloc_func_type);
    bh_assert(realloc_func_type->param_count == 4);
    bh_assert(realloc_func_type->result_count == 1);
    /* TODO: Check param/result types match (i32, i32, i32, i32) -> i32 */

    argv[0] = old_ptr;
    argv[1] = old_size;
    argv[2] = align;
    argv[3] = new_size;

    if (!wasm_runtime_call_wasm(exec_env, (WASMFunctionInstanceCommon*)realloc_func,
                                argc, argv)) {
        /* Exception occurred during realloc call */
        /* For PoC, assume 0 indicates error. A robust solution would check exec_env->cur_exception */
        LOG_DEBUG("aot_call_wasm_realloc: wasm_runtime_call_wasm failed");
        return 0; /* Allocation failed (or other error) */
    }

    return argv[0]; /* Result is in argv[0] */
}

uint16_t *
aot_transcode_utf8_to_utf16le_on_host(const char *utf8_str,
                                      uint32_t utf8_len_bytes,
                                      uint32_t *out_utf16_code_units,
                                      void *exec_env_ptr)
{
    /* Re-use existing transcoder logic.
       This function allocates memory using bh_allocator (via bh_lib_transcode_utf8_to_utf16le)
       which uses the default allocator (likely os_malloc).
       This is suitable for host-side temporary buffers.
    */
    (void)exec_env_ptr; /* exec_env might be used for finer-grained error reporting later */
    uint16_t *ret_buf;
    uint64_t utf16_len_code_units_64;

    /* wasm_component_canonical.h's transcode_utf8_to_utf16le_on_host uses bh_lib_...
       which returns BHT_OK or BHT_ERROR_BUFFER_OVERFLOW or BHT_ERROR_INVALID_UTF8
       It expects out_utf16_buf to be pre-allocated if out_utf16_buf_size > 0.
       We need a version that allocates.
       Let's look at `transcode_utf8_to_utf16le` in `wasm_runtime_common.c`
       It calculates length first, then allocates, then transcodes.
    */

    utf16_len_code_units_64 =
        bh_lib_get_utf16le_from_utf8((const uint8_t*)utf8_str, utf8_len_bytes);

    if (utf16_len_code_units_64 == (uint64_t)-1) { /* Invalid UTF8 */
        if (out_utf16_code_units) *out_utf16_code_units = 0;
        return NULL;
    }
    if (utf16_len_code_units_64 > UINT32_MAX) { /* Too large for uint32_t length */
         if (out_utf16_code_units) *out_utf16_code_units = 0;
        return NULL;
    }

    *out_utf16_code_units = (uint32_t)utf16_len_code_units_64;
    if (*out_utf16_code_units == 0 && utf8_len_bytes > 0) { /* Non-empty invalid UTF-8 can result in 0 estimated length */
        const uint8 *p = (const uint8*)utf8_str, *p_end = p + utf8_len_bytes;
        if (!bh_lib_is_valid_utf8(&p, p_end)) {
             return NULL;
        }
    }


    /* Allocate buffer for UTF-16 string. Size is code_units * 2 bytes. */
    /* Check for overflow: code_units * 2 */
    if (*out_utf16_code_units > UINT32_MAX / 2) {
        *out_utf16_code_units = 0;
        return NULL; /* Allocation size would overflow */
    }
    uint32_t buf_size_bytes = (*out_utf16_code_units) * 2;

    if (buf_size_bytes == 0 && utf8_len_bytes == 0) { /* Empty string */
        /* Return a non-NULL pointer for empty string if that's the convention, or NULL.
           Let's return a malloc'ed empty buffer of size 0 or 1 for consistency with free.
           Or, handle 0-length string specifically in the caller.
           For now, if length is 0, malloc(0) or malloc(1) might be problematic or platform-dependent.
           Let's ensure we allocate at least 1 byte if utf16_len_code_units is 0 but we need a valid ptr.
           However, the transcoder might handle this. `bh_lib_transcode_utf8_to_utf16le` expects non-NULL buffer.
        */
         if (utf8_len_bytes == 0) { /* Legit empty string */
            ret_buf = loader_malloc(1, NULL, 0); /* Allocate 1 byte to ensure a valid pointer that can be freed */
            if (ret_buf) ret_buf[0] = 0; /* Technically not needed for 0 code units */
            return ret_buf;
         }
         /* If utf8_len_bytes > 0 but out_utf16_code_units is 0, it's an error or fully non-printable string */
         /* The earlier check bh_lib_get_utf16le_from_utf8 should have returned -1 for invalid. */
         /* If it's valid but all non-printable resulting in 0 code units, still proceed. */
    }


    ret_buf = loader_malloc(buf_size_bytes, NULL, 0);
    if (!ret_buf) {
        *out_utf16_code_units = 0;
        return NULL;
    }

    /* Perform transcoding */
    if (bh_lib_transcode_utf8_to_utf16le((const uint8_t*)utf8_str, utf8_len_bytes,
                                         ret_buf, out_utf16_code_units) != BHT_OK) {
        loader_free(ret_buf);
        *out_utf16_code_units = 0;
        return NULL;
    }

    return ret_buf;
}

/* Renamed from aot_free_host_buffer */
void
aot_host_free_bytes(void *buffer, void *exec_env_ptr)
{
    (void)exec_env_ptr; /* exec_env might be used later */
    if (buffer) {
        loader_free(buffer);
    }
}


LLVMValueRef
aot_compile_lower_string_thunk(
    AOTCompContext *comp_ctx,
    AOTFuncContext *func_ctx,
    uint32_t memory_idx,
    uint32_t realloc_func_idx,
    WASMComponentCanonicalOptionKind string_encoding)
{
    LLVMContextRef context = comp_ctx->context;
    LLVMModuleRef module = comp_ctx->module;
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMValueRef func = NULL;
    LLVMTypeRef func_type_ref, param_types[5], ret_type;
    char func_name[128];

    bh_assert(memory_idx == 0 && "Currently only memory index 0 is supported for lower thunks");
    bh_assert(string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF8 ||
              string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 ||
              string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16); // Latin1 not fully handled here yet

    /*
     * Thunk signature:
     * void (*thunk_func)(WASMExecEnv *exec_env,          // param 0
     *                    char *host_str,                 // param 1
     *                    uint32_t host_str_len_bytes,   // param 2
     *                    uint32_t *out_wasm_offset,      // param 3
     *                    uint32_t *out_wasm_len_units);  // param 4
     */
    ret_type = LLVMVoidTypeInContext(context);
    param_types[0] = LLVMPointerType(comp_ctx->exec_env_type, 0);  /* WASMExecEnv* */
    param_types[1] = LLVMPointerType(LLVMInt8TypeInContext(context), 0); /* char* host_str */
    param_types[2] = LLVMInt32TypeInContext(context);                   /* uint32_t host_str_len_bytes */
    param_types[3] = LLVMPointerType(LLVMInt32TypeInContext(context), 0);/* uint32_t* out_wasm_offset */
    param_types[4] = LLVMPointerType(LLVMInt32TypeInContext(context), 0);/* uint32_t* out_wasm_len_units */

    func_type_ref = LLVMFunctionType(ret_type, param_types, 5, false);

    snprintf(func_name, sizeof(func_name), "aot_lower_string_mem%u_realloc%u_enc%d",
             memory_idx, realloc_func_idx, (int)string_encoding);

    if (!(func = LLVMAddFunction(module, func_name, func_type_ref))) {
        aot_set_last_error("LLVMAddFunction failed for lower_string thunk.");
        return NULL;
    }
    LLVMSetFunctionCallConv(func, LLVMCCallConv);

    LLVMValueRef exec_env_ptr = LLVMGetParam(func, 0);
    LLVMSetValueName(exec_env_ptr, "exec_env");
    LLVMValueRef host_str_ptr = LLVMGetParam(func, 1);
    LLVMSetValueName(host_str_ptr, "host_str");
    LLVMValueRef host_str_len_bytes_val = LLVMGetParam(func, 2);
    LLVMSetValueName(host_str_len_bytes_val, "host_str_len_bytes");
    LLVMValueRef out_wasm_offset_ptr = LLVMGetParam(func, 3);
    LLVMSetValueName(out_wasm_offset_ptr, "out_wasm_offset_ptr");
    LLVMValueRef out_wasm_len_units_ptr = LLVMGetParam(func, 4);
    LLVMSetValueName(out_wasm_len_units_ptr, "out_wasm_len_units_ptr");

    LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlockInContext(context, func, "entry");
    LLVMBasicBlockRef transcoding_bb = NULL, realloc_bb = NULL, memcpy_bb = NULL;
    LLVMBasicBlockRef cleanup_bb = NULL, finish_bb = NULL, error_bb = NULL;

    LLVMValueRef module_inst_ptr;
    LLVMValueRef source_data_ptr_val;    /* char* or uint16_t* (as i8*) */
    LLVMValueRef alloc_size_in_wasm_val; /* number of bytes to allocate in wasm */
    LLVMValueRef wasm_len_for_output_val;/* length units for wasm (bytes for utf8, code units for utf16) */
    LLVMValueRef wasm_ptr_val = NULL;    /* offset returned by realloc */
    LLVMValueRef alignment_val;

    LLVMValueRef temp_utf16_buffer_ptr = NULL; /* Stores pointer from transcoding for later free */
    LLVMValueRef actual_utf16_code_units_val = NULL; /* Stores actual code units from transcoding */

    LLVMValueRef zero_i32 = LLVMConstInt(LLVMInt32TypeInContext(context), 0, false);
    LLVMValueRef exec_env_void_ptr = LLVMBuildBitCast(builder, exec_env_ptr, LLVMPointerType(LLVMInt8TypeInContext(context),0), "exec_env_void_ptr");


    LLVMPositionBuilderAtEnd(builder, entry_bb);

    /* 1. Get WASMModuleInstance */
    module_inst_ptr = LLVMBuildLoad2(builder, comp_ctx->module_inst_type, /* Correct type for module_inst */
                                     LLVMBuildStructGEP2(builder, comp_ctx->exec_env_type, exec_env_ptr,
                                                         AOT_EXEC_ENV_MODULE_INST_OFFISET, "module_inst_addr"),
                                     "module_inst");

    /* Define error block first: sets outputs to 0 and returns */
    error_bb = LLVMAppendBasicBlockInContext(context, func, "error_handler");
    LLVMPositionBuilderAtEnd(builder, error_bb);
    /* If temp_utf16_buffer_ptr is valid, it means it was allocated and needs freeing */
    LLVMValueRef temp_buf_to_free_phi = LLVMBuildPhi(builder, LLVMPointerType(LLVMInt8TypeInContext(context),0), "temp_buf_phi");

    LLVMTypeRef free_func_param_types[] = { LLVMPointerType(LLVMInt8TypeInContext(context),0), LLVMPointerType(LLVMInt8TypeInContext(context),0) };
    LLVMTypeRef free_func_type = LLVMFunctionType(LLVMVoidTypeInContext(context), free_func_param_types, 2, false);
    LLVMValueRef free_func_ptr = LLVMGetNamedFunction(module, "aot_host_free_bytes");
    if (!free_func_ptr) {
        free_func_ptr = LLVMAddFunction(module, "aot_host_free_bytes", free_func_type);
    }
    LLVMValueRef free_args[] = { temp_buf_to_free_phi, exec_env_void_ptr };

    /* Only call free if temp_buf_to_free_phi is not NULL */
    LLVMBasicBlockRef free_needed_bb = LLVMAppendBasicBlockInContext(context, func, "free_needed");
    LLVMBasicBlockRef skip_free_bb = LLVMAppendBasicBlockInContext(context, func, "skip_free");
    LLVMValueRef is_temp_buf_null = LLVMBuildICmp(builder, LLVMIntEQ, temp_buf_to_free_phi, LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(context),0)), "is_temp_buf_null");
    LLVMBuildCondBr(builder, is_temp_buf_null, skip_free_bb, free_needed_bb);

    LLVMPositionBuilderAtEnd(builder, free_needed_bb);
    LLVMBuildCall2(builder, free_func_type, free_func_ptr, free_args, 2, "");
    LLVMBuildBr(builder, skip_free_bb);

    LLVMPositionBuilderAtEnd(builder, skip_free_bb);
    LLVMBuildStore(builder, zero_i32, out_wasm_offset_ptr);
    LLVMBuildStore(builder, zero_i32, out_wasm_len_units_ptr);
    LLVMBuildRetVoid(builder);


    /* Back to entry_bb to start real logic */
    LLVMPositionBuilderAtEnd(builder, entry_bb);

    if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 || string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16) {
        transcoding_bb = LLVMAppendBasicBlockInContext(context, func, "transcoding_utf16");
        LLVMBuildBr(builder, transcoding_bb);
        LLVMPositionBuilderAtEnd(builder, transcoding_bb);

        /* 2. Transcoding for UTF-16 */
        LLVMTypeRef transcode_param_types[] = {
            LLVMPointerType(LLVMInt8TypeInContext(context),0), /* utf8_str */
            LLVMInt32TypeInContext(context),                   /* utf8_len_bytes */
            LLVMPointerType(LLVMInt32TypeInContext(context),0),/* out_utf16_code_units_ptr */
            LLVMPointerType(LLVMInt8TypeInContext(context),0)  /* exec_env_void_ptr */
        };
        LLVMTypeRef transcode_ret_type = LLVMPointerType(LLVMInt16TypeInContext(context),0); /* uint16_t* */
        LLVMTypeRef transcode_func_type = LLVMFunctionType(transcode_ret_type, transcode_param_types, 4, false);
        LLVMValueRef transcode_func_ptr = LLVMGetNamedFunction(module, "aot_transcode_utf8_to_utf16le_on_host");
        if (!transcode_func_ptr) {
            transcode_func_ptr = LLVMAddFunction(module, "aot_transcode_utf8_to_utf16le_on_host", transcode_func_type);
        }

        /* Create a temporary stack variable for out_utf16_code_units */
        LLVMValueRef utf16_code_units_ptr_alloca = LLVMBuildAlloca(builder, LLVMInt32TypeInContext(context), "utf16_code_units_addr");
        LLVMValueRef transcode_args[] = { host_str_ptr, host_str_len_bytes_val, utf16_code_units_ptr_alloca, exec_env_void_ptr };

        temp_utf16_buffer_ptr = LLVMBuildCall2(builder, transcode_func_type, transcode_func_ptr, transcode_args, 4, "temp_utf16_buf");
        actual_utf16_code_units_val = LLVMBuildLoad2(builder, LLVMInt32TypeInContext(context), utf16_code_units_ptr_alloca, "actual_utf16_code_units");

        source_data_ptr_val = LLVMBuildBitCast(builder, temp_utf16_buffer_ptr, LLVMPointerType(LLVMInt8TypeInContext(context),0), "utf16_buf_as_i8ptr");
        wasm_len_for_output_val = actual_utf16_code_units_val;
        /* alloc_size_in_wasm = actual_utf16_code_units * 2 (bytes) */
        alloc_size_in_wasm_val = LLVMBuildMul(builder, actual_utf16_code_units_val, LLVMConstInt(LLVMInt32TypeInContext(context), 2, false), "alloc_size_utf16");
        alignment_val = LLVMConstInt(LLVMInt32TypeInContext(context), 2, false); /* UTF-16 requires 2-byte alignment */

        LLVMBasicBlockRef transcode_fail_bb = LLVMAppendBasicBlockInContext(context, func, "transcode_fail");
        realloc_bb = LLVMAppendBasicBlockInContext(context, func, "realloc_after_transcode");
        LLVMValueRef is_transcode_buf_null = LLVMBuildICmp(builder, LLVMIntEQ, temp_utf16_buffer_ptr, LLVMConstNull(transcode_ret_type), "is_transcode_buf_null");
        LLVMBuildCondBr(builder, is_transcode_buf_null, transcode_fail_bb, realloc_bb);

        LLVMPositionBuilderAtEnd(builder, transcode_fail_bb);
        LLVMAddIncoming(temp_buf_to_free_phi, &temp_utf16_buffer_ptr, &transcode_fail_bb, 1); /* It might be NULL, free handles NULL */
        LLVMBuildBr(builder, error_bb);

        LLVMPositionBuilderAtEnd(builder, realloc_bb);
    } else { /* UTF-8 */
        realloc_bb = LLVMAppendBasicBlockInContext(context, func, "realloc_utf8");
        LLVMBuildBr(builder, realloc_bb); // Branch from entry to realloc_bb for UTF-8
        LLVMPositionBuilderAtEnd(builder, realloc_bb);

        source_data_ptr_val = host_str_ptr;
        wasm_len_for_output_val = host_str_len_bytes_val;
        alloc_size_in_wasm_val = host_str_len_bytes_val;
        alignment_val = LLVMConstInt(LLVMInt32TypeInContext(context), 1, false); /* UTF-8 alignment is 1 */
        /* For error block phi node, if we go to error from here, temp_utf16_buffer_ptr is NULL */
    }

    /* Common path for realloc */
    /* PHI nodes for values that differ based on encoding path */
    LLVMValueRef source_data_phi = LLVMBuildPhi(builder, LLVMPointerType(LLVMInt8TypeInContext(context),0), "source_data_phi");
    LLVMAddIncoming(source_data_phi, &source_data_ptr_val, (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 || string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16) ? realloc_bb : entry_bb, 1);
    if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF8) { // If UTF-8, this was set in entry_bb's successor for UTF8
         LLVMAddIncoming(source_data_phi, &host_str_ptr, realloc_bb,1); // this assumes realloc_bb is the successor of entry for UTF8
    }


    LLVMValueRef alloc_size_phi = LLVMBuildPhi(builder, LLVMInt32TypeInContext(context), "alloc_size_phi");
    LLVMAddIncoming(alloc_size_phi, &alloc_size_in_wasm_val, (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 || string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16) ? realloc_bb : entry_bb,1);
     if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF8) {
         LLVMAddIncoming(alloc_size_phi, &host_str_len_bytes_val, realloc_bb,1);
    }


    LLVMValueRef final_wasm_len_phi = LLVMBuildPhi(builder, LLVMInt32TypeInContext(context), "wasm_len_phi");
    LLVMAddIncoming(final_wasm_len_phi, &wasm_len_for_output_val, (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 || string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16) ? realloc_bb : entry_bb,1);
     if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF8) {
         LLVMAddIncoming(final_wasm_len_phi, &host_str_len_bytes_val, realloc_bb,1);
    }

    LLVMValueRef final_alignment_phi = LLVMBuildPhi(builder, LLVMInt32TypeInContext(context), "alignment_phi");
    LLVMAddIncoming(final_alignment_phi, &alignment_val, (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 || string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16) ? realloc_bb : entry_bb,1);
     if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF8) {
         LLVMAddIncoming(final_alignment_phi, &alignment_val, realloc_bb,1); // alignment_val was set for UTF8 path
    }


    /* 4. Allocate Memory in Wasm (via realloc) */
    LLVMTypeRef realloc_helper_param_types[] = {
        LLVMPointerType(comp_ctx->exec_env_type, 0), /* exec_env */
        LLVMInt32TypeInContext(context), /* realloc_fidx */
        LLVMInt32TypeInContext(context), /* old_ptr (0) */
        LLVMInt32TypeInContext(context), /* old_size (0) */
        LLVMInt32TypeInContext(context), /* align */
        LLVMInt32TypeInContext(context)  /* new_size */
    };
    LLVMTypeRef realloc_helper_ret_type = LLVMInt32TypeInContext(context); /* wasm_offset */
    LLVMTypeRef realloc_helper_func_type = LLVMFunctionType(realloc_helper_ret_type, realloc_helper_param_types, 6, false);
    LLVMValueRef realloc_helper_func_ptr = LLVMGetNamedFunction(module, "aot_call_wasm_realloc");
    if (!realloc_helper_func_ptr) {
        realloc_helper_func_ptr = LLVMAddFunction(module, "aot_call_wasm_realloc", realloc_helper_func_type);
    }

    LLVMValueRef realloc_fidx_val = LLVMConstInt(LLVMInt32TypeInContext(context), realloc_func_idx, false);
    LLVMValueRef realloc_args[] = {
        exec_env_ptr, realloc_fidx_val, zero_i32, zero_i32, /* old_ptr=0, old_size=0 */
        final_alignment_phi, final_alloc_size_phi
    };
    wasm_ptr_val = LLVMBuildCall2(builder, realloc_helper_func_type, realloc_helper_func_ptr, realloc_args, 6, "wasm_offset_raw");

    memcpy_bb = LLVMAppendBasicBlockInContext(context, func, "memcpy_to_wasm");
    LLVMBasicBlockRef realloc_fail_bb = LLVMAppendBasicBlockInContext(context, func, "realloc_fail");
    LLVMValueRef is_realloc_fail = LLVMBuildICmp(builder, LLVMIntEQ, wasm_ptr_val, zero_i32, "is_realloc_fail");
    LLVMBuildCondBr(builder, is_realloc_fail, realloc_fail_bb, memcpy_bb);

    LLVMPositionBuilderAtEnd(builder, realloc_fail_bb);
    LLVMValueRef null_i8_ptr = LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(context),0));
    LLVMAddIncoming(temp_buf_to_free_phi, (string_encoding != CANONICAL_OPTION_STRING_ENCODING_UTF8) ? temp_utf16_buffer_ptr : &null_i8_ptr, &realloc_fail_bb, 1);
    LLVMBuildBr(builder, error_bb);

    /* 5. Copy String to Wasm Memory */
    LLVMPositionBuilderAtEnd(builder, memcpy_bb);
    AOTMemInfo *mem_info = &comp_ctx->memories[memory_idx];
    LLVMValueRef mem_base_ptr = LLVMBuildLoad2(builder, LLVMPointerType(LLVMInt8TypeInContext(context),0), mem_info->mem_base_addr_val, "mem_base_addr");

    LLVMValueRef wasm_dest_addr_offset;
     if (comp_ctx->pointer_size == sizeof(uint64_t)) { /* 64-bit target */
        wasm_dest_addr_offset = LLVMBuildZExt(builder, wasm_ptr_val, LLVMInt64TypeInContext(context), "wasm_ptr_i64");
    } else { /* 32-bit target */
        wasm_dest_addr_offset = wasm_ptr_val;
    }
    LLVMValueRef wasm_dest_addr = LLVMBuildGEP2(builder, LLVMInt8TypeInContext(context), mem_base_ptr, &wasm_dest_addr_offset, 1, "wasm_dest_addr");

    LLVMTypeRef memcpy_len_type = (comp_ctx->pointer_size == sizeof(uint64_t))
                                    ? LLVMInt64TypeInContext(context)
                                    : LLVMInt32TypeInContext(context);
    LLVMValueRef final_alloc_size_for_memcpy;
    if (comp_ctx->pointer_size == sizeof(uint64_t)) {
        final_alloc_size_for_memcpy = LLVMBuildZExt(builder, final_alloc_size_phi, LLVMInt64TypeInContext(context), "alloc_size_memcpy64");
    } else {
        final_alloc_size_for_memcpy = final_alloc_size_phi;
    }

    LLVMValueRef memcpy_args_lower[] = {
        wasm_dest_addr,       /* dest */
        final_source_data_phi, /* src */
        final_alloc_size_for_memcpy, /* len */
        LLVMConstInt(LLVMInt1TypeInContext(context), 0, false) /* is_volatile = false */
    };
    LLVMTypeRef memcpy_arg_types_lower[] = { LLVMPointerType(LLVMInt8TypeInContext(context),0), LLVMPointerType(LLVMInt8TypeInContext(context),0), memcpy_len_type };
    LLVMValueRef memcpy_func_lower = LLVMGetIntrinsicDeclaration(module, "llvm.memcpy", memcpy_arg_types_lower, 3);
    LLVMBuildCall2(builder, LLVMGetCalledFunctionType(memcpy_func_lower), memcpy_func_lower, memcpy_args_lower, 4, "");

    /* 6. Store Output Parameters */
    LLVMBuildStore(builder, wasm_ptr_val, out_wasm_offset_ptr);
    LLVMBuildStore(builder, final_wasm_len_phi, out_wasm_len_units_ptr);

    cleanup_bb = LLVMAppendBasicBlockInContext(context, func, "cleanup_and_finish");
    LLVMBuildBr(builder, cleanup_bb);
    LLVMPositionBuilderAtEnd(builder, cleanup_bb);

    /* 7. Cleanup (free transcoded buffer for UTF16) */
    if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 || string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16) {
        LLVMValueRef free_args_cleanup[] = { temp_utf16_buffer_ptr, exec_env_void_ptr }; /* temp_utf16_buffer_ptr is from the UTF16 path */
        LLVMBuildCall2(builder, free_func_type, free_func_ptr, free_args_cleanup, 2, "");
    }

    finish_bb = LLVMAppendBasicBlockInContext(context, func, "finish");
    LLVMBuildBr(builder, finish_bb);
    LLVMPositionBuilderAtEnd(builder, finish_bb);
    LLVMBuildRetVoid(builder);

    /* Connect entry to correct first block (realloc_bb for UTF8, transcoding_bb for UTF16) */
    LLVMPositionBuilderAtEnd(builder, entry_bb);
    if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 || string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16) {
        LLVMBuildBr(builder, transcoding_bb);
         LLVMAddIncoming(temp_buf_to_free_phi, &temp_utf16_buffer_ptr, &transcode_fail_bb, 1);
    } else { /* UTF-8 */
        LLVMBuildBr(builder, realloc_bb);
        LLVMValueRef null_i8_ptr_entry_path = LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(context),0));
        LLVMAddIncoming(temp_buf_to_free_phi, &null_i8_ptr_entry_path, &realloc_fail_bb, 1); // if realloc fails for UTF8, temp_buf is NULL

        /* Add incoming for PHI nodes from UTF8 path */
        LLVMAddIncoming(source_data_phi, &host_str_ptr, realloc_bb, 1);
        LLVMAddIncoming(alloc_size_phi, &host_str_len_bytes_val, realloc_bb, 1);
        LLVMAddIncoming(final_wasm_len_phi, &host_str_len_bytes_val, realloc_bb, 1);
        LLVMAddIncoming(final_alignment_phi, &alignment_val, realloc_bb, 1); // alignment_val set for UTF8
    }
    /* Add incoming for PHI nodes from UTF16 path */
    if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16 || string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_OR_UTF16) {
        LLVMAddIncoming(source_data_phi, &source_data_ptr_val, realloc_bb, 1); // source_data_ptr_val is set in transcoding_bb
        LLVMAddIncoming(alloc_size_phi, &alloc_size_in_wasm_val, realloc_bb, 1);
        LLVMAddIncoming(final_wasm_len_phi, &wasm_len_for_output_val, realloc_bb, 1);
        LLVMAddIncoming(final_alignment_phi, &alignment_val, realloc_bb, 1); // alignment_val set for UTF16
    }


    /* Verification and passes would go here if func_ctx is used */
    return func;
}


/* --- Thunks for Primitive Types --- */

static LLVMTypeRef
get_llvm_prim_type(AOTCompContext *comp_ctx, WASMComponentPrimValType primitive_type) {
    LLVMContextRef context = comp_ctx->context;
    switch (primitive_type) {
        case PRIM_VAL_BOOL: /* Represent bool as i32 for now */
        case PRIM_VAL_U8:
        case PRIM_VAL_S8:
        case PRIM_VAL_U16:
        case PRIM_VAL_S16:
        case PRIM_VAL_U32:
        case PRIM_VAL_S32:
        case PRIM_VAL_CHAR: /* char is u32 */
            return LLVMInt32TypeInContext(context);
        case PRIM_VAL_U64:
        case PRIM_VAL_S64:
            return LLVMInt64TypeInContext(context);
        case PRIM_VAL_F32:
            return LLVMFloatTypeInContext(context);
        case PRIM_VAL_F64:
            return LLVMDoubleTypeInContext(context);
        default:
            bh_assert(!"Invalid primitive_type for LLVM translation");
            return NULL;
    }
}

static uint32_t
get_llvm_prim_size_bytes(AOTCompContext *comp_ctx, WASMComponentPrimValType primitive_type) {
    switch (primitive_type) {
        case PRIM_VAL_BOOL: return 4; /* Assuming bool is represented as i32 on host */
        case PRIM_VAL_U8:   return 1; /* Though passed as i32 in core_value, actual size for malloc */
        case PRIM_VAL_S8:   return 1;
        case PRIM_VAL_U16:  return 2;
        case PRIM_VAL_S16:  return 2;
        case PRIM_VAL_U32:  return 4;
        case PRIM_VAL_S32:  return 4;
        case PRIM_VAL_CHAR: return 4; /* char is u32 */
        case PRIM_VAL_U64:  return 8;
        case PRIM_VAL_S64:  return 8;
        case PRIM_VAL_F32:  return 4;
        case PRIM_VAL_F64:  return 8;
        default:
            bh_assert(!"Invalid primitive_type for size calculation");
            return 0;
    }
}


LLVMValueRef
aot_compile_lift_primitive_thunk(
    AOTCompContext *comp_ctx,
    AOTFuncContext *func_ctx,
    WASMComponentPrimValType primitive_type)
{
    LLVMContextRef context = comp_ctx->context;
    LLVMModuleRef module = comp_ctx->module;
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMValueRef func = NULL;
    char func_name[128];

    LLVMTypeRef llvm_prim_type = get_llvm_prim_type(comp_ctx, primitive_type);
    if (!llvm_prim_type) return NULL;

    uint32_t prim_size_bytes = get_llvm_prim_size_bytes(comp_ctx, primitive_type);
    if (prim_size_bytes == 0) return NULL;


    /* Thunk signature: PrimType* (*thunk)(WASMExecEnv*, PrimType core_value) */
    LLVMTypeRef ret_type = LLVMPointerType(llvm_prim_type, 0);
    LLVMTypeRef param_types[] = {
        LLVMPointerType(comp_ctx->exec_env_type, 0), /* WASMExecEnv* */
        llvm_prim_type                               /* core_value */
    };
    LLVMTypeRef func_type = LLVMFunctionType(ret_type, param_types, 2, false);

    snprintf(func_name, sizeof(func_name), "aot_lift_primitive_type%d", (int)primitive_type);
    if (!(func = LLVMAddFunction(module, func_name, func_type))) {
        aot_set_last_error("LLVMAddFunction failed for lift_primitive thunk.");
        return NULL;
    }
    LLVMSetFunctionCallConv(func, LLVMCCallConv);

    LLVMValueRef exec_env_ptr = LLVMGetParam(func, 0);
    LLVMSetValueName(exec_env_ptr, "exec_env");
    LLVMValueRef core_value = LLVMGetParam(func, 1);
    LLVMSetValueName(core_value, "core_value");

    LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlockInContext(context, func, "entry");
    LLVMBasicBlockRef malloc_fail_bb = LLVMAppendBasicBlockInContext(context, func, "malloc_fail");
    LLVMBasicBlockRef store_val_bb = LLVMAppendBasicBlockInContext(context, func, "store_val");
    LLVMPositionBuilderAtEnd(builder, entry_bb);

    /* Allocate host memory */
    LLVMValueRef size_val = LLVMConstInt(LLVMInt32TypeInContext(context), prim_size_bytes, false);
    LLVMValueRef exec_env_void_ptr = LLVMBuildBitCast(builder, exec_env_ptr, LLVMPointerType(LLVMInt8TypeInContext(context),0), "exec_env_void_ptr");

    LLVMTypeRef alloc_param_types[] = { LLVMInt32TypeInContext(context), LLVMPointerType(LLVMInt8TypeInContext(context),0) };
    LLVMTypeRef alloc_ret_type = LLVMPointerType(LLVMInt8TypeInContext(context),0); /* void* */
    LLVMTypeRef alloc_func_type = LLVMFunctionType(alloc_ret_type, alloc_param_types, 2, false);
    LLVMValueRef alloc_func_ptr = LLVMGetNamedFunction(module, "aot_host_alloc_bytes");
    if (!alloc_func_ptr) {
        alloc_func_ptr = LLVMAddFunction(module, "aot_host_alloc_bytes", alloc_func_type);
    }
    LLVMValueRef alloc_args[] = { size_val, exec_env_void_ptr };
    LLVMValueRef host_mem_i8_ptr = LLVMBuildCall2(builder, alloc_func_type, alloc_func_ptr, alloc_args, 2, "host_mem_i8_ptr");

    LLVMValueRef is_alloc_null = LLVMBuildICmp(builder, LLVMIntEQ, host_mem_i8_ptr, LLVMConstNull(alloc_ret_type), "is_alloc_null");
    LLVMBuildCondBr(builder, is_alloc_null, malloc_fail_bb, store_val_bb);

    LLVMPositionBuilderAtEnd(builder, malloc_fail_bb);
    LLVMBuildRet(builder, LLVMConstNull(ret_type)); /* Return NULL if malloc failed */

    LLVMPositionBuilderAtEnd(builder, store_val_bb);
    LLVMValueRef host_mem_typed_ptr = LLVMBuildBitCast(builder, host_mem_i8_ptr, ret_type, "host_mem_typed_ptr");
    LLVMBuildStore(builder, core_value, host_mem_typed_ptr);
    LLVMBuildRet(builder, host_mem_typed_ptr);

    if (LLVMVerifyFunction(func, LLVMPrintMessageAction) != 0) {
        fprintf(stderr, "LLVMVerifyFunction failed for %s\n", func_name);
    }
    return func;
}


LLVMValueRef
aot_compile_lower_primitive_thunk(
    AOTCompContext *comp_ctx,
    AOTFuncContext *func_ctx,
    WASMComponentPrimValType primitive_type)
{
    LLVMContextRef context = comp_ctx->context;
    LLVMModuleRef module = comp_ctx->module;
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMValueRef func = NULL;
    char func_name[128];

    LLVMTypeRef llvm_prim_type = get_llvm_prim_type(comp_ctx, primitive_type);
    if (!llvm_prim_type) return NULL;

    /* Thunk signature: void (*thunk)(WASMExecEnv*, PrimType* comp_val_ptr, PrimType* out_core_val_ptr) */
    LLVMTypeRef param_types[] = {
        LLVMPointerType(comp_ctx->exec_env_type, 0), /* WASMExecEnv* */
        LLVMPointerType(llvm_prim_type, 0),          /* component_value_ptr */
        LLVMPointerType(llvm_prim_type, 0)           /* out_core_value_ptr */
    };
    LLVMTypeRef func_type = LLVMFunctionType(LLVMVoidTypeInContext(context), param_types, 3, false);

    snprintf(func_name, sizeof(func_name), "aot_lower_primitive_type%d", (int)primitive_type);
    if (!(func = LLVMAddFunction(module, func_name, func_type))) {
        aot_set_last_error("LLVMAddFunction failed for lower_primitive thunk.");
        return NULL;
    }
    LLVMSetFunctionCallConv(func, LLVMCCallConv);

    // LLVMValueRef exec_env_ptr = LLVMGetParam(func, 0); LLVMSetValueName(exec_env_ptr, "exec_env");
    LLVMGetParam(func, 0); /* exec_env, currently unused in this simple thunk */
    LLVMValueRef component_value_ptr = LLVMGetParam(func, 1);
    LLVMSetValueName(component_value_ptr, "comp_val_ptr");
    LLVMValueRef out_core_value_ptr = LLVMGetParam(func, 2);
    LLVMSetValueName(out_core_value_ptr, "out_core_val_ptr");

    LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlockInContext(context, func, "entry");
    LLVMPositionBuilderAtEnd(builder, entry_bb);

    LLVMValueRef loaded_value = LLVMBuildLoad2(builder, llvm_prim_type, component_value_ptr, "loaded_comp_val");
    LLVMBuildStore(builder, loaded_value, out_core_value_ptr);
    LLVMBuildRetVoid(builder);

    if (LLVMVerifyFunction(func, LLVMPrintMessageAction) != 0) {
        fprintf(stderr, "LLVMVerifyFunction failed for %s\n", func_name);
    }
    if (LLVMVerifyFunction(func, LLVMPrintMessageAction) != 0) {
        fprintf(stderr, "LLVMVerifyFunction failed for %s\n", func_name);
        // LLVMDumpValue(func);
    }


    return func;
}
