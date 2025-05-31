/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "aot_component_canonical.h"
#include "aot_emit_exception.h" // For aot_set_last_error and potentially error throwing
#include "llvm/Config/llvm-config.h" // For LLVM_VERSION_NUMBER

// Forward declarations for static helper functions
static LLVMValueRef
lift_primitive(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
               LLVMValueRef core_val, WASMType core_wasm_type,
               AOTCanonPrimValType primitive_kind, LLVMValueRef *error_flag);

static LLVMValueRef
lift_string(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
            LLVMValueRef str_offset, LLVMValueRef str_len,
            const AOTCanonValType *target_canon_type,
            int32_t memory_idx, int32_t realloc_func_idx, /* Options for allocation */
            LLVMValueRef *error_flag);

static LLVMValueRef
lift_list(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
          LLVMValueRef list_offset, LLVMValueRef list_len,
          const AOTCanonValType *target_canon_type,
          int32_t memory_idx, int32_t realloc_func_idx, /* Options for allocation */
          LLVMValueRef *error_flag);

// Main lifting function
LLVMValueRef
aot_canon_lift_value(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
                     LLVMValueRef *core_val_ptr, WASMType *core_wasm_type,
                     uint32_t num_core_vals,
                     const AOTCanonValType *target_canon_type,
                     LLVMValueRef *error_flag)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMValueRef result_val = NULL;
    AOTImportFunc *import_func = NULL;
    int32_t memory_idx = -1;
    int32_t realloc_func_idx = -1;

    bh_assert(error_flag && LLVMTypeOf(*error_flag) == I1_TYPE);

    if (!target_canon_type) {
        aot_set_last_error("target_canon_type is NULL.");
        LLVMBuildStore(builder, I1_ONE, *error_flag);
        return NULL;
    }

    // Placeholder: If this lift is for arguments of an import call being made from current func_ctx,
    // try to get its canonical options. This is a simplification.
    // A robust solution needs to know which function call these args/results belong to.
    // For now, this won't correctly fetch options for most cases.
    // if (func_ctx && func_ctx->aot_func && func_ctx->aot_func->import_func_idx != (uint32)-1
    //     && func_ctx->aot_func->import_func_idx < comp_ctx->comp_data->import_func_count) {
    //     import_func = &comp_ctx->comp_data->import_funcs[func_ctx->aot_func->import_func_idx];
    //     memory_idx = import_func->canon_memory_idx;
    //     realloc_func_idx = import_func->canon_realloc_func_idx;
    // }


    switch (target_canon_type->kind) {
        case AOT_CANON_TYPE_KIND_PRIMITIVE:
            if (num_core_vals != 1) {
                aot_set_last_error("Primitive lift expects 1 core value.");
                LLVMBuildStore(builder, I1_ONE, *error_flag);
                return NULL;
            }
            result_val = lift_primitive(comp_ctx, func_ctx, core_val_ptr[0],
                                        core_wasm_type[0],
                                        target_canon_type->u.primitive,
                                        error_flag);
            break;

        case AOT_CANON_TYPE_KIND_LIST:
            if (num_core_vals != 2) { // offset, len
                aot_set_last_error("List lift expects 2 core values (offset, len).");
                LLVMBuildStore(builder, I1_ONE, *error_flag);
                return NULL;
            }
            if (core_wasm_type[0] != VALUE_TYPE_I32 || core_wasm_type[1] != VALUE_TYPE_I32) {
                 aot_set_last_error("List lift core values must be I32.");
                 LLVMBuildStore(builder, I1_ONE, *error_flag);
                 return NULL;
            }
            if (target_canon_type->u.list.elem_type->kind == AOT_CANON_TYPE_KIND_PRIMITIVE
                && target_canon_type->u.list.elem_type->u.primitive == AOT_CANON_PRIM_VAL_TYPE_CHAR) {
                result_val = lift_string(comp_ctx, func_ctx, core_val_ptr[0], core_val_ptr[1],
                                         target_canon_type, memory_idx, realloc_func_idx, error_flag);
            } else {
                result_val = lift_list(comp_ctx, func_ctx, core_val_ptr[0], core_val_ptr[1],
                                       target_canon_type, memory_idx, realloc_func_idx, error_flag);
            }
            break;

        case AOT_CANON_TYPE_KIND_RECORD:
            aot_set_last_error("LIFT for Record not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_VARIANT:
            aot_set_last_error("LIFT for Variant not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_ENUM:
             if (num_core_vals != 1 || core_wasm_type[0] != VALUE_TYPE_I32) {
                aot_set_last_error("Enum lift expects 1 core I32 value.");
                LLVMBuildStore(builder, I1_ONE, *error_flag);
                return NULL;
            }
            // Enum lifting is complex: map i32 to string label, then to canonical enum representation.
            // For now, pass through as if it's a primitive S32 for placeholder.
            // result_val = lift_primitive(comp_ctx, func_ctx, core_val_ptr[0], core_wasm_type[0], AOT_CANON_PRIM_VAL_TYPE_S32, error_flag);
            aot_set_last_error("LIFT for Enum not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_OPTION:
            aot_set_last_error("LIFT for Option not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_RESULT:
            aot_set_last_error("LIFT for Result not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_RESOURCE:
             if (num_core_vals != 1 || core_wasm_type[0] != VALUE_TYPE_I32) {
                aot_set_last_error("Resource lift expects 1 core I32 value.");
                LLVMBuildStore(builder, I1_ONE, *error_flag);
                return NULL;
            }
            result_val = core_val_ptr[0];
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            break;
        default:
            aot_set_last_error("Unknown target canonical type kind for LIFT.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
    }
    return result_val;
}

static LLVMValueRef
lift_primitive(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
               LLVMValueRef core_val, WASMType core_wasm_type,
               AOTCanonPrimValType primitive_kind, LLVMValueRef *error_flag)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMTypeRef target_llvm_type = NULL;

    switch (primitive_kind) {
        case AOT_CANON_PRIM_VAL_TYPE_BOOL:
            if (core_wasm_type != VALUE_TYPE_I32) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return LLVMBuildICmp(builder, LLVMIntNE, core_val, I32_ZERO, "bool_lift");
        case AOT_CANON_PRIM_VAL_TYPE_S8:
        case AOT_CANON_PRIM_VAL_TYPE_U8:
            if (core_wasm_type != VALUE_TYPE_I32) goto type_error;
            target_llvm_type = INT8_TYPE;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return LLVMBuildTrunc(builder, core_val, target_llvm_type, "to_i8");
        case AOT_CANON_PRIM_VAL_TYPE_S16:
        case AOT_CANON_PRIM_VAL_TYPE_U16:
            if (core_wasm_type != VALUE_TYPE_I32) goto type_error;
            target_llvm_type = INT16_TYPE;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return LLVMBuildTrunc(builder, core_val, target_llvm_type, "to_i16");
        case AOT_CANON_PRIM_VAL_TYPE_S32:
        case AOT_CANON_PRIM_VAL_TYPE_U32:
        case AOT_CANON_PRIM_VAL_TYPE_CHAR:
            if (core_wasm_type != VALUE_TYPE_I32) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return core_val;
        case AOT_CANON_PRIM_VAL_TYPE_S64:
        case AOT_CANON_PRIM_VAL_TYPE_U64:
            if (core_wasm_type != VALUE_TYPE_I64) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return core_val;
        case AOT_CANON_PRIM_VAL_TYPE_F32:
            if (core_wasm_type != VALUE_TYPE_F32) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return core_val;
        case AOT_CANON_PRIM_VAL_TYPE_F64:
            if (core_wasm_type != VALUE_TYPE_F64) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return core_val;
        default:
            aot_set_last_error("Unsupported primitive type for lifting.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            return NULL;
    }
type_error:
    aot_set_last_error("Core Wasm type mismatch for primitive lifting.");
    LLVMBuildStore(builder, I1_ONE, *error_flag);
    return NULL;
}

static LLVMValueRef
lift_string(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
            LLVMValueRef str_offset, LLVMValueRef str_len,
            const AOTCanonValType *target_canon_type,
            int32_t memory_idx_opt, int32_t realloc_func_idx_opt,
            LLVMValueRef *error_flag)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    // memory_idx_opt and realloc_func_idx_opt are from the canonical options of the function
    // being lifted FOR (e.g. an import's definition if these are args for it).
    // If lifting for an export, these might be from the component's own ABI.

    LOG_VERBOSE("LIFT for String (stub): core_offset=%p, core_len=%p. MemIdx: %d, ReallocIdx: %d",
                str_offset, str_len, memory_idx_opt, realloc_func_idx_opt);

    // Placeholder: Call realloc/malloc
    // LLVMValueRef new_ptr;
    // if (realloc_func_idx_opt != -1) {
    //    LLVMValueRef realloc_func = get_func_ref_from_idx(comp_ctx, func_ctx, realloc_func_idx_opt);
    //    LLVMValueRef args[] = { LLVMConstNull(INT8_PTR_TYPE), I32_ZERO, I32_CONST(1) /*align*/, str_len /*new_size, or transcoded_len*/ };
    //    new_ptr = LLVMBuildCall(builder, realloc_func, args, 4, "lifted_str_mem");
    // } else {
    //    // Call generic runtime allocator: e.g., void* aot_runtime_alloc_string_for_host(exec_env, uint32_t len);
    //    // This needs to return a structure or pointer that the host/canonical ABI expects for strings.
    //    // For now, assume it returns {i8* ptr, i32 len} as a struct.
    // }
    // TODO: copy/transcode from Wasm memory (str_offset, str_len in default linear memory) to new_ptr.

    aot_set_last_error("LIFT for String not fully implemented (memory allocation outlined).");
    LLVMBuildStore(builder, I1_ONE, *error_flag);
    return NULL;
}

static LLVMValueRef
lift_list(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
          LLVMValueRef list_offset, LLVMValueRef list_len,
          const AOTCanonValType *target_canon_type,
          int32_t memory_idx_opt, int32_t realloc_func_idx_opt,
          LLVMValueRef *error_flag)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    LOG_VERBOSE("LIFT for List (stub): core_offset=%p, core_len=%p. MemIdx: %d, ReallocIdx: %d",
                list_offset, list_len, memory_idx_opt, realloc_func_idx_opt);

    // Placeholder: Similar to lift_string, use memory_idx_opt/realloc_func_idx_opt
    // to allocate memory for the lifted list structure and its elements.
    // Recursively call aot_canon_lift_value for each element.

    aot_set_last_error("LIFT for List not fully implemented (memory allocation outlined).");
    LLVMBuildStore(builder, I1_ONE, *error_flag);
    return NULL;
}

// Forward declarations for static helper functions for lowering
static LLVMValueRef
lower_primitive(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
                LLVMValueRef canon_val, AOTCanonPrimValType primitive_kind,
                WASMType target_core_wasm_type, LLVMValueRef *error_flag);

static LLVMValueRef
lower_string(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
             LLVMValueRef canon_str_ptr, LLVMValueRef canon_str_len,
             const AOTCanonValType *source_canon_type,
             int32_t memory_idx_opt, int32_t realloc_func_idx_opt, /* Options for allocation */
             LLVMValueRef *out_core_offset, LLVMValueRef *out_core_len,
             LLVMValueRef *error_flag);

static LLVMValueRef
lower_list(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
           LLVMValueRef canon_list_ptr, LLVMValueRef canon_list_len,
           const AOTCanonValType *source_canon_type,
           int32_t memory_idx_opt, int32_t realloc_func_idx_opt, /* Options for allocation */
           LLVMValueRef *out_core_offset, LLVMValueRef *out_core_len,
           LLVMValueRef *error_flag);


// Main lowering function
LLVMValueRef
aot_canon_lower_value(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
                      LLVMValueRef canon_val,
                      const AOTCanonValType *source_canon_type,
                      WASMType *target_core_wasm_types,
                      uint32_t *num_target_core_vals,
                      LLVMValueRef *error_flag)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMValueRef result_val = NULL;
    AOTImportFunc *import_func = NULL; // If lowering for a call to an import
    int32_t memory_idx = -1;
    int32_t realloc_func_idx = -1;

    bh_assert(error_flag && LLVMTypeOf(*error_flag) == I1_TYPE);
    bh_assert(target_core_wasm_types && num_target_core_vals);
    *num_target_core_vals = 0;

    if (!source_canon_type) {
        aot_set_last_error("source_canon_type is NULL.");
        LLVMBuildStore(builder, I1_ONE, *error_flag);
        return NULL;
    }

    // Placeholder: If this lower is for arguments of an import call being made from current func_ctx,
    // try to get its canonical options. This is a simplification.
    // This information should ideally be passed explicitly or be more readily available
    // based on whether we are lowering args for an import call, or a return for an export call.
    // if (func_ctx && func_ctx->aot_func && func_ctx->aot_func->import_func_idx != (uint32)-1
    //     && func_ctx->aot_func->import_func_idx < comp_ctx->comp_data->import_func_count) {
    //     import_func = &comp_ctx->comp_data->import_funcs[func_ctx->aot_func->import_func_idx];
    //     memory_idx = import_func->canon_memory_idx;
    //     realloc_func_idx = import_func->canon_realloc_func_idx;
    // }


    switch (source_canon_type->kind) {
        case AOT_CANON_TYPE_KIND_PRIMITIVE:
            if (!target_core_wasm_types) {
                 aot_set_last_error("target_core_wasm_types for primitive lowering is NULL.");
                 LLVMBuildStore(builder, I1_ONE, *error_flag);
                 return NULL;
            }
            result_val = lower_primitive(comp_ctx, func_ctx, canon_val,
                                         source_canon_type->u.primitive,
                                         target_core_wasm_types[0],
                                         error_flag);
            if (!LLVMConstIntGetZExtValue(LLVMBuildLoad(builder, *error_flag, "err_load_primitive_lower"))) {
                *num_target_core_vals = 1;
            }
            break;

        case AOT_CANON_TYPE_KIND_LIST:
        {
            LLVMValueRef core_offset = NULL, core_len = NULL;
            LLVMValueRef canon_ptr = LLVMBuildExtractValue(builder, canon_val, 0, "canon_val_ptr");
            LLVMValueRef canon_len_val = LLVMBuildExtractValue(builder, canon_val, 1, "canon_val_len");

            if (source_canon_type->u.list.elem_type->kind == AOT_CANON_TYPE_KIND_PRIMITIVE
                && source_canon_type->u.list.elem_type->u.primitive == AOT_CANON_PRIM_VAL_TYPE_CHAR) {
                result_val = lower_string(comp_ctx, func_ctx, canon_ptr, canon_len_val,
                                          source_canon_type, memory_idx, realloc_func_idx,
                                          &core_offset, &core_len, error_flag);
            } else {
                result_val = lower_list(comp_ctx, func_ctx, canon_ptr, canon_len_val,
                                        source_canon_type, memory_idx, realloc_func_idx,
                                        &core_offset, &core_len, error_flag);
            }

            if (!LLVMConstIntGetZExtValue(LLVMBuildLoad(builder, *error_flag, "err_load_list_lower"))) {
                target_core_wasm_types[0] = VALUE_TYPE_I32;
                target_core_wasm_types[1] = VALUE_TYPE_I32;
                *num_target_core_vals = 2;
            }
            break;
        }
        case AOT_CANON_TYPE_KIND_RECORD:
            aot_set_last_error("LOWER for Record not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_VARIANT:
            aot_set_last_error("LOWER for Variant not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_ENUM:
             if (target_core_wasm_types[0] != VALUE_TYPE_I32) {
                aot_set_last_error("Enum lower target must be I32.");
                LLVMBuildStore(builder, I1_ONE, *error_flag);
                return NULL;
            }
            // Enum lowering is complex: map canonical enum to string label, then to i32.
            // For now, pass through as if it's a primitive S32.
            // result_val = lower_primitive(comp_ctx, func_ctx, canon_val, AOT_CANON_PRIM_VAL_TYPE_S32, target_core_wasm_types[0], error_flag);
            aot_set_last_error("LOWER for Enum not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_OPTION:
            aot_set_last_error("LOWER for Option not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_RESULT:
            aot_set_last_error("LOWER for Result not yet implemented.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
        case AOT_CANON_TYPE_KIND_RESOURCE:
            if (target_core_wasm_types[0] != VALUE_TYPE_I32) {
                aot_set_last_error("Resource lower target must be I32.");
                LLVMBuildStore(builder, I1_ONE, *error_flag);
                return NULL;
            }
            result_val = canon_val;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            *num_target_core_vals = 1;
            target_core_wasm_types[0] = VALUE_TYPE_I32;
            break;
        default:
            aot_set_last_error("Unknown source canonical type kind for LOWER.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            result_val = NULL;
            break;
    }
    return result_val;
}


static LLVMValueRef
lower_primitive(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
                LLVMValueRef canon_val, AOTCanonPrimValType primitive_kind,
                WASMType target_core_wasm_type, LLVMValueRef *error_flag)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    switch (primitive_kind) {
        case AOT_CANON_PRIM_VAL_TYPE_BOOL:
            if (target_core_wasm_type != VALUE_TYPE_I32) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return LLVMBuildZExt(builder, canon_val, I32_TYPE, "bool_to_i32");
        case AOT_CANON_PRIM_VAL_TYPE_S8:
        case AOT_CANON_PRIM_VAL_TYPE_U8:
            if (target_core_wasm_type != VALUE_TYPE_I32) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            if (primitive_kind == AOT_CANON_PRIM_VAL_TYPE_S8)
                return LLVMBuildSExt(builder, canon_val, I32_TYPE, "i8_to_i32");
            else
                return LLVMBuildZExt(builder, canon_val, I32_TYPE, "u8_to_i32");
        case AOT_CANON_PRIM_VAL_TYPE_S16:
        case AOT_CANON_PRIM_VAL_TYPE_U16:
            if (target_core_wasm_type != VALUE_TYPE_I32) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            if (primitive_kind == AOT_CANON_PRIM_VAL_TYPE_S16)
                return LLVMBuildSExt(builder, canon_val, I32_TYPE, "i16_to_i32");
            else
                return LLVMBuildZExt(builder, canon_val, I32_TYPE, "u16_to_i32");
        case AOT_CANON_PRIM_VAL_TYPE_S32:
        case AOT_CANON_PRIM_VAL_TYPE_U32:
        case AOT_CANON_PRIM_VAL_TYPE_CHAR:
            if (target_core_wasm_type != VALUE_TYPE_I32) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return canon_val;
        case AOT_CANON_PRIM_VAL_TYPE_S64:
        case AOT_CANON_PRIM_VAL_TYPE_U64:
            if (target_core_wasm_type != VALUE_TYPE_I64) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return canon_val;
        case AOT_CANON_PRIM_VAL_TYPE_F32:
            if (target_core_wasm_type != VALUE_TYPE_F32) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return canon_val;
        case AOT_CANON_PRIM_VAL_TYPE_F64:
            if (target_core_wasm_type != VALUE_TYPE_F64) goto type_error;
            LLVMBuildStore(builder, I1_ZERO, *error_flag);
            return canon_val;
        default:
            aot_set_last_error("Unsupported primitive type for lowering.");
            LLVMBuildStore(builder, I1_ONE, *error_flag);
            return NULL;
    }
type_error:
    aot_set_last_error("Target core Wasm type mismatch for primitive lowering.");
    LLVMBuildStore(builder, I1_ONE, *error_flag);
    return NULL;
}

static LLVMValueRef
lower_string(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
             LLVMValueRef canon_str_ptr, LLVMValueRef canon_str_len,
             const AOTCanonValType *source_canon_type,
             int32_t memory_idx_opt, int32_t realloc_func_idx_opt,
             LLVMValueRef *out_core_offset, LLVMValueRef *out_core_len,
             LLVMValueRef *error_flag)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    LOG_VERBOSE("LOWER for String (stub): canon_ptr=%p, canon_len=%p. MemIdx: %d, ReallocIdx: %d",
                canon_str_ptr, canon_str_len, memory_idx_opt, realloc_func_idx_opt);

    // Placeholder:
    // 1. Determine target memory (memory_idx_opt, default to 0 if -1).
    // 2. Determine realloc function (realloc_func_idx_opt).
    //    LLVMValueRef realloc_func = get_llvm_funcref_for_realloc(comp_ctx, func_ctx, realloc_func_idx_opt, target_module_instance);
    //    If !realloc_func:
    //        // Call generic runtime allocator: e.g.,
    //        // void* aot_runtime_alloc_in_target_memory(exec_env, target_module_inst, size, memory_idx);
    //        // This needs to be callable.
    //        // For now, error out if specific realloc is not found.
    //        aot_set_last_error("LOWER for String: Target realloc function not available.");
    //        LLVMBuildStore(builder, I1_ONE, *error_flag);
    //        *out_core_offset = LLVMGetUndef(I32_TYPE); *out_core_len = LLVMGetUndef(I32_TYPE);
    //        return LLVMGetUndef(LLVMStructTypeInContext(comp_ctx->llvm_context, (LLVMTypeRef[]){I32_TYPE, I32_TYPE}, 2, false));
    //
    // 3. LLVMValueRef alloc_size = canon_str_len; // Or transcoded size.
    //    LLVMValueRef args[] = { LLVMConstNull(INT8_PTR_TYPE) /*old_ptr*/, I32_ZERO /*old_size*/,
    //                            I32_CONST(1) /*align, assuming 1 for strings*/, alloc_size };
    //    *out_core_offset = LLVMBuildCall(builder, realloc_func, args, 4, "lowered_str_ptr");
    //    *out_core_len = alloc_size;
    //    // Check for allocation failure (e.g., if realloc returns 0).
    //    // TODO: Copy/transcode from canon_str_ptr to *out_core_offset using target memory.

    aot_set_last_error("LOWER for String not fully implemented (memory allocation outlined).");
    LLVMBuildStore(builder, I1_ONE, *error_flag);
    *out_core_offset = LLVMGetUndef(I32_TYPE);
    *out_core_len = LLVMGetUndef(I32_TYPE);

    LLVMTypeRef members[] = { I32_TYPE, I32_TYPE };
    LLVMTypeRef struct_type = LLVMStructTypeInContext(comp_ctx->llvm_context, members, 2, false);
    return LLVMGetUndef(struct_type);
}

static LLVMValueRef
lower_list(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
           LLVMValueRef canon_list_ptr, LLVMValueRef canon_list_len,
           const AOTCanonValType *source_canon_type,
           int32_t memory_idx_opt, int32_t realloc_func_idx_opt,
           LLVMValueRef *out_core_offset, LLVMValueRef *out_core_len,
           LLVMValueRef *error_flag)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    LOG_VERBOSE("LOWER for List (stub): canon_ptr=%p, canon_len=%p. MemIdx: %d, ReallocIdx: %d",
                canon_list_ptr, canon_list_len, memory_idx_opt, realloc_func_idx_opt);

    // Placeholder: Similar to lower_string, use memory_idx_opt/realloc_func_idx_opt
    // to allocate memory in the target component's linear memory.
    // Recursively call aot_canon_lower_value for each element and store.

    aot_set_last_error("LOWER for List not fully implemented (memory allocation outlined).");
    LLVMBuildStore(builder, I1_ONE, *error_flag);
    *out_core_offset = LLVMGetUndef(I32_TYPE);
    *out_core_len = LLVMGetUndef(I32_TYPE);

    LLVMTypeRef members[] = { I32_TYPE, I32_TYPE };
    LLVMTypeRef struct_type = LLVMStructTypeInContext(comp_ctx->llvm_context, members, 2, false);
    return LLVMGetUndef(struct_type);
}

// Helper to create a placeholder canonical type for demonstration
// In a real scenario, this would come from component metadata
static AOTCanonValType*
get_component_canon_type_for_param(AOTCompContext *comp_ctx, AOTImportFunc *import_func, uint32 param_idx) {
    // TODO: This function should look up the *actual* canonical type definition
    // for the given parameter of the imported component function using comp_ctx->component_target.
    // The AOTImportFunc might need to store an index to its canonical type definition
    // within the component's type sections.
    // For now, it remains a placeholder based on the core Wasm type.
    if (!import_func || param_idx >= import_func->func_type->param_count) return NULL;
    WASMType wasm_type = import_func->func_type->types[param_idx];

    AOTCanonValType *canon_type = wasm_runtime_malloc(sizeof(AOTCanonValType));
    if (!canon_type) return NULL;

    switch (wasm_type) {
        case VALUE_TYPE_I32:
            canon_type->kind = AOT_CANON_TYPE_KIND_PRIMITIVE;
            canon_type->u.primitive = AOT_CANON_PRIM_VAL_TYPE_S32; break;
        case VALUE_TYPE_I64:
            canon_type->kind = AOT_CANON_TYPE_KIND_PRIMITIVE;
            canon_type->u.primitive = AOT_CANON_PRIM_VAL_TYPE_S64; break;
        case VALUE_TYPE_F32:
            canon_type->kind = AOT_CANON_TYPE_KIND_PRIMITIVE;
            canon_type->u.primitive = AOT_CANON_PRIM_VAL_TYPE_F32; break;
        case VALUE_TYPE_F64:
            canon_type->kind = AOT_CANON_TYPE_KIND_PRIMITIVE;
            canon_type->u.primitive = AOT_CANON_PRIM_VAL_TYPE_F64; break;
        default: wasm_runtime_free(canon_type); return NULL;
    }
    return canon_type;
}

static AOTCanonValType*
get_component_canon_type_for_result(AOTCompContext *comp_ctx, AOTImportFunc *import_func) {
    // TODO: Similar to params, look up the *actual* canonical type for the result.
    if (!import_func || import_func->func_type->result_count == 0) return NULL;
    WASMType wasm_type = import_func->func_type->results[0]; // Assuming single result for now

    AOTCanonValType *canon_type = wasm_runtime_malloc(sizeof(AOTCanonValType));
    if (!canon_type) return NULL;

    switch (wasm_type) {
        case VALUE_TYPE_I32:
            canon_type->kind = AOT_CANON_TYPE_KIND_PRIMITIVE;
            canon_type->u.primitive = AOT_CANON_PRIM_VAL_TYPE_S32; break;
        case VALUE_TYPE_I64:
            canon_type->kind = AOT_CANON_TYPE_KIND_PRIMITIVE;
            canon_type->u.primitive = AOT_CANON_PRIM_VAL_TYPE_S64; break;
        case VALUE_TYPE_F32:
            canon_type->kind = AOT_CANON_TYPE_KIND_PRIMITIVE;
            canon_type->u.primitive = AOT_CANON_PRIM_VAL_TYPE_F32; break;
        case VALUE_TYPE_F64:
            canon_type->kind = AOT_CANON_TYPE_KIND_PRIMITIVE;
            canon_type->u.primitive = AOT_CANON_PRIM_VAL_TYPE_F64; break;
        default: wasm_runtime_free(canon_type); return NULL;
    }
    return canon_type;
}


LLVMValueRef
aot_get_or_create_component_call_wrapper(AOTCompContext *comp_ctx,
                                         AOTFuncContext *caller_func_ctx, /* From where the call is made */
                                         uint32 import_func_idx)
{
    AOTImportFunc *import_func;
    AOTFuncType *core_func_type;
    char wrapper_name[128];
    LLVMValueRef wrapper_func;
    LLVMTypeRef wrapper_llvm_func_type;
    LLVMTypeRef *wrapper_param_llvm_types = NULL;
    LLVMTypeRef wrapper_ret_llvm_type;
    uint32 i;

    bh_assert(comp_ctx && caller_func_ctx);
    bh_assert(import_func_idx < comp_ctx->comp_data->import_func_count);

    import_func = &comp_ctx->comp_data->import_funcs[import_func_idx];

    if (!import_func->is_cross_component_call) {
        aot_set_last_error("Attempted to create component call wrapper for a non-component call.");
        return NULL;
    }

    core_func_type = import_func->func_type;

    // Construct unique wrapper name
    snprintf(wrapper_name, sizeof(wrapper_name), "aot_component_wrapper_idx_%u", import_func_idx);

    // Check if wrapper already exists in the LLVM module
    wrapper_func = LLVMGetNamedFunction(caller_func_ctx->module, wrapper_name);
    if (wrapper_func) {
        // TODO: Verify existing function signature matches? Or assume it's correct.
        return wrapper_func;
    }

    // --- Create the wrapper function ---

    // 1. Determine LLVM signature for the wrapper (matches core Wasm import)
    uint32 wrapper_param_count = core_func_type->param_count + 1; // +1 for exec_env
    wrapper_param_llvm_types = wasm_runtime_malloc(sizeof(LLVMTypeRef) * wrapper_param_count);
    if (!wrapper_param_llvm_types) {
        aot_set_last_error("Memory allocation failed for wrapper_param_llvm_types.");
        goto fail_cleanup;
    }

    wrapper_param_llvm_types[0] = comp_ctx->exec_env_type; // First param is always exec_env
    for (i = 0; i < core_func_type->param_count; ++i) {
        wrapper_param_llvm_types[i + 1] = TO_LLVM_TYPE(core_func_type->types[i]);
    }

    if (core_func_type->result_count > 0) {
        // Assuming single return for now, multi-value returns need struct_type or similar
        wrapper_ret_llvm_type = TO_LLVM_TYPE(core_func_type->results[0]);
    } else {
        wrapper_ret_llvm_type = VOID_TYPE;
    }

    wrapper_llvm_func_type = LLVMFunctionType(wrapper_ret_llvm_type, wrapper_param_llvm_types, wrapper_param_count, false);
    if (!wrapper_llvm_func_type) {
        aot_set_last_error("Failed to create LLVM function type for wrapper.");
        goto fail_cleanup;
    }

    // 2. Create the LLVM function for the wrapper
    wrapper_func = LLVMAddFunction(caller_func_ctx->module, wrapper_name, wrapper_llvm_func_type);
    if (!wrapper_func) {
        aot_set_last_error("Failed to add LLVM function for wrapper.");
        goto fail_cleanup;
    }
    // Set linkage to ensure it's available. Internal or LinkOnceODR might be options.
    // LLVMSetLinkage(wrapper_func, LLVMInternalLinkage); // Or other as appropriate

    // 3. Create entry basic block
    LLVMBasicBlockRef entry_block = LLVMAppendBasicBlockInContext(comp_ctx->context, wrapper_func, "entry");
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMPositionBuilderAtEnd(builder, entry_block);

    LLVMValueRef error_flag_ptr = LLVMBuildAlloca(builder, I1_TYPE, "error_flag_ptr");
    LLVMBuildStore(builder, I1_ZERO, error_flag_ptr);

    LLVMValueRef *lifted_args = NULL; // Changed name from lowered_args for clarity
    uint32 num_lifted_args = 0;
    if (core_func_type->param_count > 0) {
        lifted_args = wasm_runtime_malloc(sizeof(LLVMValueRef) * core_func_type->param_count * 2);
        if (!lifted_args) {
            aot_set_last_error("Memory allocation failed for lifted_args.");
            goto fail_emit_body;
        }

        for (i = 0; i < core_func_type->param_count; ++i) {
            LLVMValueRef core_arg = LLVMGetParam(wrapper_func, i + 1);
            AOTCanonValType *target_canon_param_type = get_component_canon_type_for_param(comp_ctx, import_func, i);
            if (!target_canon_param_type) {
                 aot_set_last_error("Failed to get component canonical type for param.");
                 goto fail_emit_body;
            }

            LLVMValueRef lifted_val = aot_canon_lift_value(comp_ctx, caller_func_ctx,
                                                           &core_arg, &core_func_type->types[i], 1,
                                                           target_canon_param_type, error_flag_ptr);
            wasm_runtime_free(target_canon_param_type);

            if (LLVMConstIntGetZExtValue(LLVMBuildLoad(builder, error_flag_ptr, "error_check_lift_arg"))) {
                 aot_set_last_error("Failed to lift argument in wrapper.");
                 goto fail_emit_body;
            }
            lifted_args[num_lifted_args++] = lifted_val;
        }
    }

    LLVMTypeRef *canon_param_llvm_types = NULL;
    if (core_func_type->param_count > 0) {
        canon_param_llvm_types = wasm_runtime_malloc(sizeof(LLVMTypeRef) * num_lifted_args); // Use num_lifted_args
        if (!canon_param_llvm_types) {
            aot_set_last_error("Mem alloc failed for canon_param_llvm_types");
            goto fail_emit_body;
        }
        for(i = 0; i < num_lifted_args; ++i) { // Use num_lifted_args
            if (lifted_args[i])
                canon_param_llvm_types[i] = LLVMTypeOf(lifted_args[i]);
            else {
                 aot_set_last_error("Lifted argument is null.");
                 goto fail_emit_body;
            }
        }
    }

    LLVMTypeRef canon_ret_llvm_type;
    AOTCanonValType *source_canon_return_type = NULL;
    if (core_func_type->result_count > 0) {
        source_canon_return_type = get_component_canon_type_for_result(comp_ctx, import_func);
        if (!source_canon_return_type) {
            aot_set_last_error("Failed to get component canonical type for result.");
            goto fail_emit_body;
        }
        if (source_canon_return_type->kind == AOT_CANON_TYPE_KIND_PRIMITIVE
            && source_canon_return_type->u.primitive == AOT_CANON_PRIM_VAL_TYPE_BOOL) {
            canon_ret_llvm_type = I1_TYPE;
        } else {
            canon_ret_llvm_type = TO_LLVM_TYPE(core_func_type->results[0]);
        }
    } else {
        canon_ret_llvm_type = VOID_TYPE;
    }

    LLVMTypeRef target_func_llvm_type = LLVMFunctionType(canon_ret_llvm_type, canon_param_llvm_types, num_lifted_args, false); // Use num_lifted_args
    if (!target_func_llvm_type) {
        aot_set_last_error("Failed to create LLVM func type for target canonical call.");
        goto fail_emit_body;
    }
    if (canon_param_llvm_types) wasm_runtime_free(canon_param_llvm_types);
    canon_param_llvm_types = NULL;

    LLVMValueRef target_func_ptr = import_func->func_ptr_linked;
    if (!target_func_ptr) {
        char target_symbol_name[256];
        snprintf(target_symbol_name, sizeof(target_symbol_name), "%s.%s_canon_abi", import_func->module_name, import_func->func_name);
        target_func_ptr = LLVMGetNamedFunction(caller_func_ctx->module, target_symbol_name);
        if (!target_func_ptr) {
            target_func_ptr = LLVMAddFunction(caller_func_ctx->module, target_symbol_name, target_func_llvm_type);
        }
    }

    LLVMValueRef canonical_ret_val = LLVMBuildCall2(builder, target_func_llvm_type, target_func_ptr,
                                                   lifted_args, num_lifted_args, // Use num_lifted_args
                                                   core_func_type->result_count > 0 ? "canon_call_ret" : "");
    if (!canonical_ret_val && core_func_type->result_count > 0) {
         if (LLVMGetLastError()) {
            aot_set_last_error(LLVMGetLastError());
            LLVMDisposeMessage(LLVMGetLastError());
            goto fail_emit_body;
         }
    }

    if (core_func_type->result_count > 0) {
        WASMType final_core_ret_type_array[1];
        uint32_t num_final_core_vals = 0;
        final_core_ret_type_array[0] = core_func_type->results[0];

        LLVMValueRef final_core_ret_val = aot_canon_lower_value(comp_ctx, caller_func_ctx,
                                                                canonical_ret_val,
                                                                source_canon_return_type,
                                                                final_core_ret_type_array,
                                                                &num_final_core_vals,
                                                                error_flag_ptr);
        if (source_canon_return_type) wasm_runtime_free(source_canon_return_type);
        source_canon_return_type = NULL;

        if (LLVMConstIntGetZExtValue(LLVMBuildLoad(builder, error_flag_ptr, "error_check_lower_ret"))) {
             aot_set_last_error("Failed to lower return value in wrapper.");
             if (!final_core_ret_val) final_core_ret_val = LLVMGetUndef(wrapper_ret_llvm_type);
        }
        LLVMBuildRet(builder, final_core_ret_val);
    } else {
        if (source_canon_return_type) wasm_runtime_free(source_canon_return_type);
        LLVMBuildRetVoid(builder);
    }

    if (lifted_args) wasm_runtime_free(lifted_args);
    if (wrapper_param_llvm_types) wasm_runtime_free(wrapper_param_llvm_types);
    return wrapper_func;

fail_emit_body:
    if (lifted_args) wasm_runtime_free(lifted_args);
    if (source_canon_return_type) wasm_runtime_free(source_canon_return_type);
fail_cleanup:
    if (wrapper_param_llvm_types) wasm_runtime_free(wrapper_param_llvm_types);
    if (canon_param_llvm_types) wasm_runtime_free(canon_param_llvm_types);
    return NULL;
}
