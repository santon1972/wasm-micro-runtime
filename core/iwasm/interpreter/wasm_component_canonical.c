/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_canonical.h"
#include "wasm_runtime.h"       /* for wasm_runtime_invoke_native, wasm_runtime_get_module_inst, etc. */
#include "wasm_memory.h"        /* for wasm_runtime_validate_app_addr, etc. */
#include "../common/wasm_component.h" /* For COMPONENT_MODEL_VERSION, etc. */
#include "bh_log.h"
#include "bh_platform.h"        /* For os_printf, strlen, memset */
#include "wasm_loader_common.h" /* For loader_malloc, loader_free */
#include <string.h> // For memset


// Helper to set error messages
static void
set_canon_error(char *error_buf, uint32 error_buf_size, const char *message)
{
    if (error_buf) {
        snprintf(error_buf, error_buf_size, "Canonical ABI error: %s", message);
    }
}

static void
set_canon_error_v(char *error_buf, uint32 error_buf_size, const char *format, ...)
{
    va_list args;
    char buf[128];

    if (error_buf) {
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        snprintf(error_buf, error_buf_size, "Canonical ABI error: %s", buf);
    }
}

// Forward declaration
static uint32 get_core_wasm_primitive_size(uint8 core_type_tag);
static uint8 get_core_wasm_type_for_primitive(WASMComponentPrimValType prim_val_type);
static bool get_component_type_core_abi_details(const WASMComponentValType *val_type,
                                                WASMModuleInstance *module_inst,
                                                uint32 *out_size, uint32 *out_alignment,
                                                char* error_buf, uint32 error_buf_size);
static uint32 align_up(uint32 val, uint32 alignment);


/* Resource Handling Globals */
#define MAX_RESOURCE_HANDLES 128 // Example size
static WAMRHostResourceEntry global_resource_table[MAX_RESOURCE_HANDLES];
static uint32_t next_resource_handle = 1; // Start handles from 1 for easier debugging (0 can be invalid)
static bool resource_table_initialized = false;

static void initialize_resource_table() {
    if (!resource_table_initialized) {
        memset(global_resource_table, 0, sizeof(global_resource_table));
        if (MAX_RESOURCE_HANDLES > 0) {
             global_resource_table[0].is_active = false; 
        }
        resource_table_initialized = true;
    }
}


bool
wasm_component_canon_lift_value(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def,
    uint32 core_func_idx,
    void *core_value_ptr, 
    uint8 core_value_type, 
    const WASMComponentValType *target_component_valtype,
    void **lifted_component_value_ptr, 
    char *error_buf, uint32 error_buf_size)
{
    WASMModuleInstance *module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32 mem_idx = (uint32)-1;

    if (!module_inst && target_component_valtype->kind != VAL_TYPE_KIND_PRIMITIVE) {
        // Primitives might not need module_inst if core_value_ptr is direct
        // For strings, lists, records (if from memory), module_inst is needed.
        set_canon_error(error_buf, error_buf_size, "Failed to get module instance from exec_env for non-primitive type.");
        return false;
    }

    if (canonical_def) { 
        for (uint32 i = 0; i < canonical_def->option_count; ++i) {
            if (canonical_def->options[i].kind == CANONICAL_OPTION_MEMORY_IDX) {
                mem_idx = canonical_def->options[i].value;
            } 
        }
    }

    *lifted_component_value_ptr = NULL; 

    if (target_component_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
        switch (target_component_valtype->u.primitive) {
            case PRIM_VAL_S32:
            case PRIM_VAL_U32:
                if (core_value_type == VALUE_TYPE_I32) {
                    uint32 *val = loader_malloc(sizeof(uint32), error_buf, error_buf_size);
                    if (!val) return false;
                    *val = *(uint32*)core_value_ptr;
                    *lifted_component_value_ptr = val;
                    return true;
                } else {
                    set_canon_error_v(error_buf, error_buf_size, "Type mismatch: core type %u for component i32", core_value_type);
                    return false;
                }
            case PRIM_VAL_S64:
            case PRIM_VAL_U64:
                if (core_value_type == VALUE_TYPE_I64) {
                    uint64 *val = loader_malloc(sizeof(uint64), error_buf, error_buf_size);
                    if (!val) return false;
                    *val = *(uint64*)core_value_ptr;
                    *lifted_component_value_ptr = val;
                    return true;
                } else {
                     set_canon_error_v(error_buf, error_buf_size, "Type mismatch: core type %u for component i64", core_value_type);
                    return false;
                }
            case PRIM_VAL_F32:
                if (core_value_type == VALUE_TYPE_F32) {
                    float32 *val = loader_malloc(sizeof(float32), error_buf, error_buf_size);
                    if (!val) return false;
                    *val = *(float32*)core_value_ptr;
                    *lifted_component_value_ptr = val;
                    return true;
                } else {
                    set_canon_error_v(error_buf, error_buf_size, "Type mismatch: core type %u for component f32", core_value_type);
                    return false;
                }
            case PRIM_VAL_F64:
                 if (core_value_type == VALUE_TYPE_F64) {
                    float64 *val = loader_malloc(sizeof(float64), error_buf, error_buf_size);
                    if (!val) return false;
                    *val = *(float64*)core_value_ptr;
                    *lifted_component_value_ptr = val;
                    return true;
                } else {
                    set_canon_error_v(error_buf, error_buf_size, "Type mismatch: core type %u for component f64", core_value_type);
                    return false;
                }
            
            case PRIM_VAL_STRING:
            {
                if (mem_idx == (uint32)-1 && canonical_def) { 
                    set_canon_error(error_buf, error_buf_size, "String lifting requires memory option if using canonical_def.");
                    return false;
                }
                if (!module_inst) { // Should have been caught earlier if canonical_def was present
                     set_canon_error(error_buf, error_buf_size, "Module instance required for string lifting from memory.");
                     return false;
                }
                
                uint32 *core_params = (uint32*)core_value_ptr; 
                uint32 offset = core_params[0];
                uint32 length = core_params[1];

                uint8 *core_mem_base = wasm_runtime_get_memory_ptr(module_inst, mem_idx, NULL);
                if (!core_mem_base) {
                     set_canon_error(error_buf, error_buf_size, "Failed to get memory pointer for string lifting.");
                     return false;
                }
                if (!wasm_runtime_validate_app_addr(module_inst, mem_idx, offset, length)) {
                    set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for string at offset %u, length %u", offset, length);
                    return false;
                }
                                
                char *lifted_str_ptr = loader_malloc(length + 1, error_buf, error_buf_size);
                if (!lifted_str_ptr) return false; 

                bh_memcpy_s(lifted_str_ptr, length + 1, core_mem_base + offset, length);
                lifted_str_ptr[length] = '\0';
                
                char **result_ptr_loc = loader_malloc(sizeof(char*), error_buf, error_buf_size);
                if (!result_ptr_loc) { loader_free(lifted_str_ptr); return false; }
                *result_ptr_loc = lifted_str_ptr;
                *lifted_component_value_ptr = result_ptr_loc;
                
                return true;
            }
            default:
                set_canon_error_v(error_buf, error_buf_size, "Unsupported primitive type for lifting: %d", target_component_valtype->u.primitive);
                return false;
        }
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_LIST) {
        if (mem_idx == (uint32)-1 && canonical_def) {
            set_canon_error(error_buf, error_buf_size, "List lifting requires memory option if using canonical_def.");
            return false;
        }
         if (!module_inst) {
             set_canon_error(error_buf, error_buf_size, "Module instance required for list lifting from memory.");
             return false;
        }

        uint32 *core_params = (uint32*)core_value_ptr;
        uint32 list_offset = core_params[0];
        uint32 list_length = core_params[1]; 

        WASMComponentValType *element_valtype = target_component_valtype->u.list.element_valtype;
        if (element_valtype->kind != VAL_TYPE_KIND_PRIMITIVE) {
            set_canon_error(error_buf, error_buf_size, "List lifting currently only supports primitive elements.");
            return false;
        }
        
        uint8 core_element_type_tag = get_core_wasm_type_for_primitive(element_valtype->u.primitive);
        uint32 core_element_size = get_core_wasm_primitive_size(core_element_type_tag);

        if (core_element_type_tag == VALUE_TYPE_VOID || core_element_size == 0) { 
            set_canon_error_v(error_buf, error_buf_size, "Unsupported/unknown list element primitive type for size calculation: %d", element_valtype->u.primitive);
            return false;
        }

        uint8 *core_mem_base = wasm_runtime_get_memory_ptr(module_inst, mem_idx, NULL);
        if (!core_mem_base) {
             set_canon_error(error_buf, error_buf_size, "Failed to get memory pointer for list lifting.");
             return false;
        }
        if (list_length > 0 && !wasm_runtime_validate_app_addr(module_inst, mem_idx, list_offset, list_length * core_element_size)) {
            set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for list at offset %u, length %u, element_size %u", list_offset, list_length, core_element_size);
            return false;
        }

        void **lifted_elements_array = NULL;
        if (list_length > 0) {
            lifted_elements_array = loader_malloc(list_length * sizeof(void*), error_buf, error_buf_size);
            if (!lifted_elements_array) return false;
            memset(lifted_elements_array, 0, list_length * sizeof(void*));

            for (uint32 i = 0; i < list_length; ++i) {
                void *core_elem_addr = core_mem_base + list_offset + (i * core_element_size);
                if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                    core_elem_addr, core_element_type_tag,
                                                    element_valtype,
                                                    &lifted_elements_array[i],
                                                    error_buf, error_buf_size)) {
                    for (uint32 j = 0; j < i; ++j) {
                        if (lifted_elements_array[j]) loader_free(lifted_elements_array[j]);
                    }
                    loader_free(lifted_elements_array);
                    return false;
                }
            }
        }
        *lifted_component_value_ptr = lifted_elements_array;
        return true;
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_RECORD) {
        WASMComponentRecordType *record_type = &target_component_valtype->u.record;
        void **lifted_fields_array = NULL;

        if (record_type->field_count == 0) {
            *lifted_component_value_ptr = NULL; // Or a special marker for empty record if needed by host
            return true;
        }

        lifted_fields_array = loader_malloc(record_type->field_count * sizeof(void*), error_buf, error_buf_size);
        if (!lifted_fields_array) return false;
        memset(lifted_fields_array, 0, record_type->field_count * sizeof(void*));

        // Assumption: core_value_ptr is a pointer to the Wasm offset of the flat record structure in memory.
        // This aligns with how tuples are lifted.
        if (mem_idx == (uint32)-1 && canonical_def) {
             set_canon_error(error_buf, error_buf_size, "Record lifting from memory requires memory option.");
             loader_free(lifted_fields_array);
             return false;
        }
        if (!module_inst) {
             set_canon_error(error_buf, error_buf_size, "Module instance required for record lifting from memory.");
             loader_free(lifted_fields_array);
             return false;
        }
        uint8 *core_mem_base = wasm_runtime_get_memory_ptr(module_inst, mem_idx, NULL);
        if (!core_mem_base) {
             set_canon_error(error_buf, error_buf_size, "Failed to get memory pointer for record lifting.");
             loader_free(lifted_fields_array);
             return false;
        }

        uint32 record_offset_in_wasm = *(uint32*)core_value_ptr;
        uint32 current_offset_within_record = 0;

        for (uint32 i = 0; i < record_type->field_count; ++i) {
            WASMComponentValType *field_val_type = record_type->fields[i].valtype;
            uint8 field_core_type_tag_for_recursive_lift = VALUE_TYPE_VOID;
            uint32 field_core_size_from_helper = 0;
            uint32 field_core_alignment_from_helper = 0;

            if (!get_component_type_core_abi_details(field_val_type, module_inst,
                                                    &field_core_size_from_helper, &field_core_alignment_from_helper,
                                                    error_buf, error_buf_size)) {
                for (uint32 j = 0; j < i; ++j) if (lifted_fields_array[j]) loader_free(lifted_fields_array[j]);
                loader_free(lifted_fields_array);
                return false; // Error set by helper
            }

            if (field_val_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
                field_core_type_tag_for_recursive_lift = get_core_wasm_type_for_primitive(field_val_type->u.primitive);
            } else {
                // Complex types are stored as offsets/pairs, recursive lift expects pointer to this data.
                field_core_type_tag_for_recursive_lift = VALUE_TYPE_I32;
            }
            
            current_offset_within_record = align_up(current_offset_within_record, field_core_alignment_from_helper);
            void *core_field_data_ptr_in_wasm = core_mem_base + record_offset_in_wasm + current_offset_within_record;

            if (!wasm_runtime_validate_app_addr(module_inst, mem_idx, record_offset_in_wasm + current_offset_within_record, field_core_size_from_helper)) {
                set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for record field %u at offset %u, size %u", i, record_offset_in_wasm + current_offset_within_record, field_core_size_from_helper);
                for (uint32 j = 0; j < i; ++j) if (lifted_fields_array[j]) loader_free(lifted_fields_array[j]);
                loader_free(lifted_fields_array);
                return false;
            }

            if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                core_field_data_ptr_in_wasm,
                                                field_core_type_tag_for_recursive_lift,
                                                field_val_type,
                                                &lifted_fields_array[i],
                                                error_buf, error_buf_size)) {
                for (uint32 j = 0; j < i; ++j) if (lifted_fields_array[j]) loader_free(lifted_fields_array[j]);
                loader_free(lifted_fields_array);
                set_canon_error_v(error_buf, error_buf_size, "Failed to lift record field %u", i);
                return false;
            }
            current_offset_within_record += field_core_size_from_helper;
        }
        *lifted_component_value_ptr = lifted_fields_array;
        return true;
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_ENUM) {
        // Enums are represented as i32 discriminants in core Wasm.
        // core_value_ptr points to this i32.
        if (core_value_type != VALUE_TYPE_I32) {
            set_canon_error_v(error_buf, error_buf_size, "Enum lifting expects core type I32, got %u", core_value_type);
            return false;
        }
        uint32 *lifted_enum_val = loader_malloc(sizeof(uint32), error_buf, error_buf_size);
        if (!lifted_enum_val) return false;
        
        *lifted_enum_val = *(uint32*)core_value_ptr;
        *lifted_component_value_ptr = lifted_enum_val;
        return true;
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_OPTION) {
        // Host representation: WAMRHostOption { uint32_t is_some; void* val; }
        // Core Wasm: i32 discriminant, then aligned payload if is_some=1
        // core_value_ptr points to the start of the Wasm option structure (i.e., the discriminant)

        if (!module_inst) { // Needed for get_component_type_core_abi_details context if not primitive
            set_canon_error(error_buf, error_buf_size, "Module instance required for option lifting.");
            return false;
        }
        
        uint32 disc = *(uint32*)core_value_ptr; // Read discriminant

        WAMRHostGeneralValue *host_option = loader_malloc(sizeof(WAMRHostGeneralValue), error_buf, error_buf_size);
        if (!host_option) return false;
        host_option->disc = disc;
        host_option->val = NULL;

        if (disc == 1) { // is_some
            WASMComponentOptionType *option_type = &target_component_valtype->u.option;
            WASMComponentValType *payload_valtype = option_type->valtype;
            
            uint32 payload_wasm_size, payload_wasm_align;
            if (!get_component_type_core_abi_details(payload_valtype, module_inst, 
                                                    &payload_wasm_size, &payload_wasm_align, 
                                                    error_buf, error_buf_size)) {
                loader_free(host_option);
                return false; // Error set by helper
            }

            uint32 discriminant_size = sizeof(uint32); // Size of the 'is_some' flag
            uint32 payload_offset_in_wasm_struct = align_up(discriminant_size, payload_wasm_align);
            void *payload_wasm_addr = (uint8*)core_value_ptr + payload_offset_in_wasm_struct;
            
            // Determine core type of payload for recursive call (usually I32 if it's an offset to another structure)
            uint8 payload_core_type_tag = VALUE_TYPE_I32; 
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                 payload_core_type_tag = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }

            if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                payload_wasm_addr, payload_core_type_tag,
                                                payload_valtype, 
                                                &host_option->val, // Store lifted payload ptr here
                                                error_buf, error_buf_size)) {
                loader_free(host_option);
                // Error message is already set by the recursive call.
                return false;
            }
        } else if (disc != 0) { // Invalid discriminant
            loader_free(host_option);
            set_canon_error_v(error_buf, error_buf_size, "Invalid discriminant %u for option type", disc);
            return false;
        }
        
        *lifted_component_value_ptr = host_option;
        return true;
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_RESULT) {
        // Host: WAMRHostGeneralValue { uint32_t disc (0=ok, 1=err); void* val; }
        // Core Wasm: i32 discriminant, then aligned payload (largest of ok/err)
        // core_value_ptr points to the discriminant in Wasm memory.
        if (!module_inst) {
            set_canon_error(error_buf, error_buf_size, "Module instance required for result lifting.");
            return false;
        }

        uint32 disc = *(uint32*)core_value_ptr; // Read discriminant

        WAMRHostGeneralValue *host_result = loader_malloc(sizeof(WAMRHostGeneralValue), error_buf, error_buf_size);
        if (!host_result) return false;
        host_result->disc = disc;
        host_result->val = NULL;

        WASMComponentResultType *result_type = &target_component_valtype->u.result;
        WASMComponentValType *payload_valtype = NULL;

        if (disc == 0) { // ok
            payload_valtype = result_type->ok_valtype;
        } else if (disc == 1) { // err
            payload_valtype = result_type->err_valtype;
        } else {
            loader_free(host_result);
            set_canon_error_v(error_buf, error_buf_size, "Invalid discriminant %u for result type", disc);
            return false;
        }

        if (payload_valtype) { // If there's a payload for this case (e.g. not result<_,void> or result<void,_>)
            uint32 overall_result_size, overall_result_align; // For the whole result<T,E>
            uint32 ok_size = 0, ok_align = 1;
            uint32 err_size = 0, err_align = 1;
            uint32 max_payload_align_for_calc = 1;

            // Need max alignment of potential payloads to calculate payload area offset
            if (result_type->ok_valtype) get_component_type_core_abi_details(result_type->ok_valtype, module_inst, &ok_size, &ok_align, error_buf, error_buf_size); // Ignore error, just need align
            if (result_type->err_valtype) get_component_type_core_abi_details(result_type->err_valtype, module_inst, &err_size, &err_align, error_buf, error_buf_size); // Ignore error
            max_payload_align_for_calc = ok_align > err_align ? ok_align : err_align;
            if (max_payload_align_for_calc == 0) max_payload_align_for_calc = 1; // Ensure non-zero

            uint32 discriminant_size = sizeof(uint32);
            uint32 payload_offset_in_wasm_struct = align_up(discriminant_size, max_payload_align_for_calc);
            void *payload_wasm_addr = (uint8*)core_value_ptr + payload_offset_in_wasm_struct;

            uint8 payload_core_type_tag = VALUE_TYPE_I32;
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                 payload_core_type_tag = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }

            if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                payload_wasm_addr, payload_core_type_tag,
                                                payload_valtype, 
                                                &host_result->val,
                                                error_buf, error_buf_size)) {
                loader_free(host_result);
                // Error message is already set by the recursive call.
                return false;
            }
        }
        *lifted_component_value_ptr = host_result;
        return true;
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_VARIANT) {
        // Host: WAMRHostGeneralValue { uint32_t disc (case index); void* val; }
        // Core Wasm: i32 discriminant, then aligned payload (largest of all cases)
        // core_value_ptr points to the discriminant in Wasm memory.
        if (!module_inst) {
            set_canon_error(error_buf, error_buf_size, "Module instance required for variant lifting.");
            return false;
        }

        uint32 disc = *(uint32*)core_value_ptr; // Read discriminant

        WAMRHostGeneralValue *host_variant = loader_malloc(sizeof(WAMRHostGeneralValue), error_buf, error_buf_size);
        if (!host_variant) return false;
        host_variant->disc = disc;
        host_variant->val = NULL;

        WASMComponentVariantType *variant_type = &target_component_valtype->u.variant;

        if (disc >= variant_type->case_count) {
            loader_free(host_variant);
            set_canon_error_v(error_buf, error_buf_size, "Invalid discriminant %u for variant type with %u cases", disc, variant_type->case_count);
            return false;
        }

        WASMComponentCase *active_case = &variant_type->cases[disc];
        WASMComponentValType *payload_valtype = active_case->valtype;

        if (payload_valtype) { // If this case has a payload
            uint32 max_case_payload_align = 1;
            // Recalculate max_case_payload_align (as done in get_component_type_core_abi_details for variant)
            // to correctly determine the payload area's alignment.
            for (uint32 i = 0; i < variant_type->case_count; ++i) {
                if (variant_type->cases[i].valtype) {
                    uint32 case_s, case_a;
                    if (get_component_type_core_abi_details(variant_type->cases[i].valtype, module_inst, &case_s, &case_a, error_buf, error_buf_size)) {
                        if (case_a > max_case_payload_align) max_case_payload_align = case_a;
                    } else { /* Ignore error for non-active cases, main check is for active_case->valtype */ }
                }
            }
            if (max_case_payload_align == 0) max_case_payload_align = 1;


            uint32 discriminant_size = sizeof(uint32);
            uint32 payload_offset_in_wasm_struct = align_up(discriminant_size, max_case_payload_align);
            void *payload_wasm_addr = (uint8*)core_value_ptr + payload_offset_in_wasm_struct;
            
            uint8 payload_core_type_tag = VALUE_TYPE_I32;
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                 payload_core_type_tag = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }

            if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                payload_wasm_addr, payload_core_type_tag,
                                                payload_valtype, 
                                                &host_variant->val,
                                                error_buf, error_buf_size)) {
                loader_free(host_variant);
                // Error message is already set by the recursive call.
                return false; 
            }
        }
        *lifted_component_value_ptr = host_variant;
        return true;
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_OWN_TYPE_IDX ||
               target_component_valtype->kind == VAL_TYPE_KIND_BORROW_TYPE_IDX) {
        // Resource handles are represented as i32 in core Wasm.
        // core_value_ptr points to this i32 handle.
        // Host representation is also a direct uint32 handle.
        if (core_value_type != VALUE_TYPE_I32) {
            set_canon_error_v(error_buf, error_buf_size, "Resource handle lifting expects core type I32, got %u", core_value_type);
            return false;
        }
        uint32 *lifted_handle_val = loader_malloc(sizeof(uint32), error_buf, error_buf_size);
        if (!lifted_handle_val) return false;
        
        *lifted_handle_val = *(uint32*)core_value_ptr;
        
        // Optional: Validate if the handle is active if desired, though typically lifting
        // might just pass the integer value. For OWN, it implies it should be valid.
        // For BORROW, it's a handle Wasm already knows.
        // initialize_resource_table(); // Ensure table is ready if validation is added
        // if (*lifted_handle_val == 0 || *lifted_handle_val >= MAX_RESOURCE_HANDLES || !global_resource_table[*lifted_handle_val].is_active) {
        //     LOG_DEBUG("Lifted resource handle %u which is not currently active in global table.", *lifted_handle_val);
        //     // Depending on strictness, this could be an error, especially for OWN.
        //     // For now, allow lifting, validity checked when used.
        // }

        *lifted_component_value_ptr = lifted_handle_val;
        return true;
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_TUPLE) {
        WASMComponentTupleType *tuple_type = &target_component_valtype->u.tuple;
        void **lifted_elements_array = NULL;

        if (tuple_type->field_count > 0) {
            lifted_elements_array = loader_malloc(tuple_type->field_count * sizeof(void*), error_buf, error_buf_size);
            if (!lifted_elements_array) return false;
            memset(lifted_elements_array, 0, tuple_type->field_count * sizeof(void*));

            // Assumption: core_value_ptr points to the start of a flat structure in Wasm memory
            // representing the tuple. Elements are laid out sequentially.
            // This requires calculating offsets.
            uint8 *current_wasm_ptr = (uint8*)core_value_ptr; 
            // This assumption is strong: core_value_ptr is a direct pointer into Wasm linear memory.
            // This might not be true if tuples are passed by reference via an offset, in which case
            // core_value_ptr would be &offset and core_value_type would be I32.
            // For now, let's assume core_value_ptr is the direct address in Wasm memory.
            // This is inconsistent with how Records are handled (void**).
            // Let's reconsider: if lifting a tuple that is part of a function signature,
            // core_value_ptr could be a pointer to where the tuple's representation (e.g. an i32 offset) is stored.
            // Or, if it's part of another structure, it could be an embedded value.

            // To be consistent with Record lifting, let's assume core_value_ptr for a tuple
            // is also a void** if it's a standalone tuple being lifted.
            // However, the subtask implies tuples might be flat in memory.
            // Let's assume for lifting, if a core function returns a tuple, it's returned as a pointer to a flat structure.
            // So core_value_ptr is that pointer (e.g. an i32 value that is an offset).
            // If core_value_type is I32, then *(uint32*)core_value_ptr is the offset.

            if (mem_idx == (uint32)-1 && canonical_def) {
                 set_canon_error(error_buf, error_buf_size, "Tuple lifting from memory requires memory option.");
                 loader_free(lifted_elements_array);
                 return false;
            }
            if (!module_inst) {
                 set_canon_error(error_buf, error_buf_size, "Module instance required for tuple lifting from memory.");
                 loader_free(lifted_elements_array);
                 return false;
            }
            uint8 *core_mem_base = wasm_runtime_get_memory_ptr(module_inst, mem_idx, NULL);
            if (!core_mem_base) {
                 set_canon_error(error_buf, error_buf_size, "Failed to get memory pointer for tuple lifting.");
                 loader_free(lifted_elements_array);
                 return false;
            }

            // Assuming core_value_ptr contains the offset of the tuple in wasm memory
            uint32 tuple_offset_in_wasm = *(uint32*)core_value_ptr;
            uint32 current_offset_within_tuple = 0;
            // overall_max_alignment_for_tuple is not strictly needed here as tuple_offset_in_wasm should already be aligned by the producer.

            for (uint32 i = 0; i < tuple_type->field_count; ++i) {
                WASMComponentValType *element_val_type = tuple_type->fields[i].valtype;
                uint8 element_core_type_tag_for_recursive_lift = VALUE_TYPE_VOID;
                uint32 element_core_size_from_helper = 0;
                uint32 element_core_alignment_from_helper = 0;

                if (!get_component_type_core_abi_details(element_val_type, module_inst,
                                                        &element_core_size_from_helper, &element_core_alignment_from_helper,
                                                        error_buf, error_buf_size)) {
                    for (uint32 j = 0; j < i; ++j) if (lifted_elements_array[j]) loader_free(lifted_elements_array[j]);
                    loader_free(lifted_elements_array);
                    // error_buf is set by get_component_type_core_abi_details
                    return false;
                }

                if (element_val_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
                    element_core_type_tag_for_recursive_lift = get_core_wasm_type_for_primitive(element_val_type->u.primitive);
                } else {
                    // For complex types (lists, strings, records, tuples) that are fields,
                    // they are stored as offsets (i32) or (offset, len) pairs (i32, i32)
                    // in the flat layout. The recursive lift call will take this offset/pair.
                    // The core_value_type for the recursive lift should indicate the type of the data stored
                    // at core_element_data_ptr_in_wasm (e.g. i32 for an offset or the first part of a pair).
                    element_core_type_tag_for_recursive_lift = VALUE_TYPE_I32;
                }

                current_offset_within_tuple = align_up(current_offset_within_tuple, element_core_alignment_from_helper);
                
                void *core_element_data_ptr_in_wasm = core_mem_base + tuple_offset_in_wasm + current_offset_within_tuple;

                if (!wasm_runtime_validate_app_addr(module_inst, mem_idx, tuple_offset_in_wasm + current_offset_within_tuple, element_core_size_from_helper)) {
                    set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for tuple element %u at offset %u, size %u", i, tuple_offset_in_wasm + current_offset_within_tuple, element_core_size_from_helper);
                    for (uint32 j = 0; j < i; ++j) if (lifted_elements_array[j]) loader_free(lifted_elements_array[j]);
                    loader_free(lifted_elements_array);
                    return false;
                }

                if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                    core_element_data_ptr_in_wasm, 
                                                    element_core_type_tag_for_recursive_lift, 
                                                    element_val_type,
                                                    &lifted_elements_array[i],
                                                    error_buf, error_buf_size)) {
                    for (uint32 j = 0; j < i; ++j) if (lifted_elements_array[j]) loader_free(lifted_elements_array[j]);
                    loader_free(lifted_elements_array);
                    set_canon_error_v(error_buf, error_buf_size, "Failed to lift tuple element %u", i);
                    return false;
                }
                current_offset_within_tuple += element_core_size_from_helper;
            }
        }
        *lifted_component_value_ptr = lifted_elements_array;
        return true;
    }

    set_canon_error_v(error_buf, error_buf_size, "Unsupported type kind for lifting: %d", target_component_valtype->kind);
    return false;
}


typedef struct HostComponentList {
    void **elements; 
    uint32_t count;
} HostComponentList;

bool
wasm_component_canon_lower_value(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def,
    uint32 core_func_idx,
    void *component_value_ptr, 
    const WASMComponentValType *source_component_valtype,
    uint8 target_core_wasm_type, 
    void *core_value_write_ptr,  
    char *error_buf, uint32 error_buf_size)
{
    WASMModuleInstance *module_inst = wasm_runtime_get_module_inst(exec_env);
    uint32 mem_idx = (uint32)-1; // Default to invalid memory index
    uint32 realloc_func_idx = (uint32)-1; // Default to no realloc function
    bool use_wasm_realloc = false;

    if (!module_inst && source_component_valtype->kind != VAL_TYPE_KIND_PRIMITIVE) {
        set_canon_error(error_buf, error_buf_size, "Failed to get module instance from exec_env for lowering non-primitive.");
        return false;
    }
    
    if (canonical_def) { 
        for (uint32 i = 0; i < canonical_def->option_count; ++i) {
            if (canonical_def->options[i].kind == CANONICAL_OPTION_MEMORY_IDX) {
                mem_idx = canonical_def->options[i].value;
            } else if (canonical_def->options[i].kind == CANONICAL_OPTION_REALLOC_FUNC_IDX) {
                realloc_func_idx = canonical_def->options[i].value;
            }
        }
        // Only use wasm_realloc if both memory and realloc_func are specified.
        // Memory is crucial for converting offset to native ptr.
        if (realloc_func_idx != (uint32)-1 && mem_idx != (uint32)-1 && module_inst) {
             // Further check: realloc_func_idx must be within defined function bounds
            if (realloc_func_idx < module_inst->function_count) { // Simple check, might need module->import_function_count vs defined_function_count
                use_wasm_realloc = true;
            } else {
                LOG_WARNING("Canonical realloc_func_idx %u is out of bounds.", realloc_func_idx);
                // Proceed without wasm_realloc
            }
        }
    }

    if (source_component_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
        switch (source_component_valtype->u.primitive) {
            case PRIM_VAL_S32:
            case PRIM_VAL_U32:
                if (target_core_wasm_type == VALUE_TYPE_I32) {
                    *(uint32*)core_value_write_ptr = *(uint32*)component_value_ptr;
                    return true;
                } else {
                    set_canon_error_v(error_buf, error_buf_size, "Type mismatch: component i32 to core type %u", target_core_wasm_type);
                    return false;
                }
            case PRIM_VAL_S64:
            case PRIM_VAL_U64:
                if (target_core_wasm_type == VALUE_TYPE_I64) {
                    *(uint64*)core_value_write_ptr = *(uint64*)component_value_ptr;
                    return true;
                } else {
                     set_canon_error_v(error_buf, error_buf_size, "Type mismatch: component i64 to core type %u", target_core_wasm_type);
                    return false;
                }
            case PRIM_VAL_F32:
                if (target_core_wasm_type == VALUE_TYPE_F32) {
                    *(float32*)core_value_write_ptr = *(float32*)component_value_ptr;
                    return true;
                } else {
                    set_canon_error_v(error_buf, error_buf_size, "Type mismatch: component f32 to core type %u", target_core_wasm_type);
                    return false;
                }
            case PRIM_VAL_F64:
                 if (target_core_wasm_type == VALUE_TYPE_F64) {
                    *(float64*)core_value_write_ptr = *(float64*)component_value_ptr;
                    return true;
                } else {
                    set_canon_error_v(error_buf, error_buf_size, "Type mismatch: component f64 to core type %u", target_core_wasm_type);
                    return false;
                }
            case PRIM_VAL_STRING:
            {
                if (target_core_wasm_type != VALUE_TYPE_I32 && target_core_wasm_type != VALUE_TYPE_I64) {
                    set_canon_error_v(error_buf, error_buf_size, "String lowering expects target for (offset,len) pair (i32 or i64), got core type %u", target_core_wasm_type);
                    return false;
                }
                if (mem_idx == (uint32)-1 && canonical_def) {
                    set_canon_error(error_buf, error_buf_size, "String lowering requires memory option if using canonical_def.");
                    return false;
                }
                 if (!module_inst) {
                     set_canon_error(error_buf, error_buf_size, "Module instance required for string lowering to memory.");
                     return false;
                }

                char *str_to_lower = *(char**)component_value_ptr; 
                if (!str_to_lower) {
                    set_canon_error(error_buf, error_buf_size, "Cannot lower null string pointer.");
                    return false;
                }
                uint32 str_len = strlen(str_to_lower);
                uint32 alloc_size = str_len; 

                uint32 wasm_offset = 0;
                void *alloc_native_ptr = NULL;

                if (use_wasm_realloc) {
                    uint32 argv[4];
                    argv[0] = 0; // old_ptr
                    argv[1] = 0; // old_size
                    argv[2] = 1; // alignment for char (u8)
                    argv[3] = alloc_size; // new_size

                    WASMFunctionInstance *realloc_func = NULL;
                    if (realloc_func_idx < module_inst->import_function_count) {
                         // This is an imported realloc, which is fine.
                         realloc_func = module_inst->import_functions[realloc_func_idx].func_ptr_linked;
                    } else {
                         realloc_func = &module_inst->functions[realloc_func_idx - module_inst->import_function_count];
                    }

                    if (!wasm_runtime_call_wasm(exec_env, realloc_func, 4, argv)) {
                        set_canon_error_v(error_buf, error_buf_size, "Wasm realloc function call failed for string. Error: %s", wasm_runtime_get_exception(module_inst));
                        return false;
                    }
                    wasm_offset = argv[0]; // Result is in the first cell
                    if (wasm_offset == 0 && alloc_size > 0) { // Realloc failed to allocate if it returns 0 and size > 0
                        set_canon_error(error_buf, error_buf_size, "Wasm realloc returned 0 for string allocation.");
                        return false;
                    }
                    alloc_native_ptr = wasm_runtime_addr_app_to_native(module_inst, mem_idx, wasm_offset);
                    if (!alloc_native_ptr && alloc_size > 0) { // Check if offset is valid
                        set_canon_error_v(error_buf, error_buf_size, "Wasm realloc returned invalid offset %u for string.", wasm_offset);
                        return false;
                    }
                } else {
                    alloc_native_ptr = wasm_runtime_module_malloc(module_inst, alloc_size, (void**)&wasm_offset); 
                    if (!alloc_native_ptr && alloc_size > 0) { // Check alloc_size > 0 because malloc(0) can return non-null
                        set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for string using module_malloc.", alloc_size);
                        return false;
                    }
                }
                
                // For zero-length strings, alloc_native_ptr might be NULL (if malloc(0) returns NULL)
                // or point to a zero-sized region. wasm_offset could be 0 or some other value.
                // The spec often treats pointers to zero-sized things carefully.
                // bh_memcpy_s handles src_len = 0 correctly.
                if (alloc_native_ptr || alloc_size == 0) { // Only copy if pointer is valid or if nothing to copy
                    bh_memcpy_s(alloc_native_ptr, alloc_size, str_to_lower, str_len);
                } else if (alloc_size > 0) { // Should have been caught by checks above
                     set_canon_error(error_buf, error_buf_size, "Internal error: alloc_native_ptr is null for non-zero string size after allocation attempt.");
                     return false;
                }


                uint32_t *out_pair = (uint32_t*)core_value_write_ptr;
                out_pair[0] = wasm_offset;
                out_pair[1] = str_len;
                
                LOG_VERBOSE("Lowered string to wasm mem offset %u, length %u", wasm_offset, str_len);
                return true;
            }
            default:
                set_canon_error_v(error_buf, error_buf_size, "Unsupported primitive type for lowering: %d", source_component_valtype->u.primitive);
                return false;
        }
    } else if (source_component_valtype->kind == VAL_TYPE_KIND_LIST) {
        if (mem_idx == (uint32)-1 && canonical_def) {
            set_canon_error(error_buf, error_buf_size, "List lowering requires memory option if using canonical_def.");
            return false;
        }
        if (!module_inst) {
             set_canon_error(error_buf, error_buf_size, "Module instance required for list lowering to memory.");
             return false;
        }

        HostComponentList *host_list = (HostComponentList*)component_value_ptr;
        if (!host_list || (!host_list->elements && host_list->count > 0)) {
            set_canon_error(error_buf, error_buf_size, "Invalid host list for lowering.");
            return false;
        }
        
        WASMComponentValType *element_valtype = source_component_valtype->u.list.element_valtype;
        if (element_valtype->kind != VAL_TYPE_KIND_PRIMITIVE) {
            set_canon_error(error_buf, error_buf_size, "List lowering currently only supports primitive elements.");
            return false;
        }

        uint8 core_element_type_tag = get_core_wasm_type_for_primitive(element_valtype->u.primitive);
        uint32 core_element_size = get_core_wasm_primitive_size(core_element_type_tag);

        if (core_element_type_tag == VALUE_TYPE_VOID || core_element_size == 0) {
            set_canon_error_v(error_buf, error_buf_size, "Could not determine core size for lowering list element type %u", element_valtype->u.primitive);
            return false;
        }

        uint32 total_wasm_alloc_size = host_list->count * core_element_size; // Assuming no complex padding, direct multiplication
        uint32 wasm_list_offset = 0;
        void *wasm_list_native_ptr = NULL;

        if (host_list->count > 0) {
            if (use_wasm_realloc) {
                uint32 list_alignment = core_element_size; // Simplification: using element size as alignment.
                                                           // Should use get_component_type_core_abi_details for element_valtype's alignment.
                                                           // For primitive lists, this is often correct.
                // TODO: Get proper alignment for list elements using get_component_type_core_abi_details(element_valtype, ...)
                // For now, using core_element_size which is a decent proxy for primitives.
                // If element_valtype is complex, its alignment might differ from its flat size.
                // Let's assume element_valtype is primitive as per current list limitations.
                // uint32 list_element_true_align = 1;
                // if(!get_component_type_core_abi_details(element_valtype, module_inst, &core_element_size, &list_element_true_align, error_buf, error_buf_size)){
                //    return false; // Error set by helper
                // }
                // Using core_element_size as alignment for now.

                uint32 argv[4];
                argv[0] = 0; // old_ptr
                argv[1] = 0; // old_size
                argv[2] = list_alignment; 
                argv[3] = total_wasm_alloc_size; // new_size
                
                WASMFunctionInstance *realloc_func = NULL;
                 if (realloc_func_idx < module_inst->import_function_count) {
                     realloc_func = module_inst->import_functions[realloc_func_idx].func_ptr_linked;
                } else {
                     realloc_func = &module_inst->functions[realloc_func_idx - module_inst->import_function_count];
                }

                if (!wasm_runtime_call_wasm(exec_env, realloc_func, 4, argv)) {
                    set_canon_error_v(error_buf, error_buf_size, "Wasm realloc function call failed for list. Error: %s", wasm_runtime_get_exception(module_inst));
                    return false;
                }
                wasm_list_offset = argv[0];
                if (wasm_list_offset == 0 && total_wasm_alloc_size > 0) {
                    set_canon_error(error_buf, error_buf_size, "Wasm realloc returned 0 for list allocation.");
                    return false;
                }
                wasm_list_native_ptr = wasm_runtime_addr_app_to_native(module_inst, mem_idx, wasm_list_offset);
                if (!wasm_list_native_ptr && total_wasm_alloc_size > 0) {
                     set_canon_error_v(error_buf, error_buf_size, "Wasm realloc returned invalid offset %u for list.", wasm_list_offset);
                     return false;
                }

            } else {
                wasm_list_native_ptr = wasm_runtime_module_malloc(module_inst, total_wasm_alloc_size, (void**)&wasm_list_offset);
                if (!wasm_list_native_ptr && total_wasm_alloc_size > 0) {
                    set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for list using module_malloc.", total_wasm_alloc_size);
                    return false;
                }
            }
        } else { // host_list->count == 0
            // wasm_list_offset remains 0, wasm_list_native_ptr remains NULL. This is fine for empty lists.
        }
        
        for (uint32 i = 0; i < host_list->count; ++i) {
            void *host_elem_ptr = host_list->elements[i];
            void *core_elem_write_ptr_in_wasm = (uint8*)wasm_list_native_ptr + (i * core_element_size);
            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_elem_ptr, element_valtype,
                                                  core_element_type_tag,
                                                  core_elem_write_ptr_in_wasm, 
                                                  error_buf, error_buf_size)) {
                // If lowering an element fails, free the already allocated list memory
                if (wasm_list_native_ptr && host_list->count > 0 && total_wasm_alloc_size > 0) {
                     if (use_wasm_realloc) {
                        // The canonical ABI doesn't specify a 'free' via realloc(ptr, old_size, alignment, 0).
                        // Some reallocs might free if new_size is 0, others might not.
                        // WAMR's default mgtc allocator does free on realloc with new_size = 0.
                        // However, to be safe and avoid depending on specific realloc behavior not mandated
                        // by the component model spec for the canonical 'realloc' option,
                        // we will consider this memory "leaked" by the guest if the guest's realloc
                        // doesn't free it and a sub-operation fails.
                        // If a canonical 'free' option were available, it would be used here.
                        LOG_WARNING("Partial list lowering failed after Wasm realloc. Wasm allocated memory at offset %u might be leaked if guest realloc doesn't handle this.", wasm_list_offset);
                     } else {
                        wasm_runtime_module_free(module_inst, wasm_list_offset);
                     }
                }
                // Error message should be set by the recursive call
                return false;
            }
        }

        uint32_t *out_pair = (uint32_t*)core_value_write_ptr;
        out_pair[0] = wasm_list_offset;
        out_pair[1] = host_list->count; 
        
        LOG_VERBOSE("Lowered list to wasm mem offset %u, element_count %u", wasm_list_offset, host_list->count);
        return true;
    } else if (source_component_valtype->kind == VAL_TYPE_KIND_RECORD) {
        WASMComponentRecordType *record_type = &source_component_valtype->u.record;
        void **component_fields_ptrs = (void**)component_value_ptr; // Array of host pointers to field values

        if (!module_inst) {
             set_canon_error(error_buf, error_buf_size, "Module instance required for record lowering to memory.");
             return false;
        }
        if (mem_idx == (uint32)-1 && canonical_def) {
            set_canon_error(error_buf, error_buf_size, "Record lowering requires memory option if using canonical_def.");
            return false;
        }

        if (record_type->field_count == 0) { // Handle empty record
            if (target_core_wasm_type == VALUE_TYPE_I32) {
                *(uint32*)core_value_write_ptr = 0; // Or a specific agreed-upon offset for empty structs, often 0 or an alignment pad.
                                                   // For now, 0, assuming it won't be dereferenced.
            } else {
                 set_canon_error_v(error_buf, error_buf_size, "Empty record lowering expects target I32 for offset, got %u", target_core_wasm_type);
                 return false;
            }
            LOG_VERBOSE("Lowered empty record to wasm mem offset 0");
            return true;
        }
        
        // 1. Calculate total size and alignment for the flat Wasm memory structure
        uint32 total_wasm_alloc_size = 0;
        uint32 max_alignment = 1;
        uint32 *field_core_sizes = NULL;
        uint32 *field_core_alignments = NULL;
        bool abi_details_success = true;

        field_core_sizes = loader_malloc(record_type->field_count * sizeof(uint32), error_buf, error_buf_size);
        if (!field_core_sizes) { return false; } // Error set by loader_malloc or implied
        field_core_alignments = loader_malloc(record_type->field_count * sizeof(uint32), error_buf, error_buf_size);
        if (!field_core_alignments) { loader_free(field_core_sizes); return false; }

        for (uint32 i = 0; i < record_type->field_count; ++i) {
            WASMComponentValType *field_val_type = record_type->fields[i].valtype;
            if (!get_component_type_core_abi_details(field_val_type, module_inst,
                                                    &field_core_sizes[i], &field_core_alignments[i],
                                                    error_buf, error_buf_size)) {
                abi_details_success = false;
                break;
            }
            if (field_core_alignments[i] > max_alignment) {
                max_alignment = field_core_alignments[i];
            }
            total_wasm_alloc_size = align_up(total_wasm_alloc_size, field_core_alignments[i]);
            total_wasm_alloc_size += field_core_sizes[i];
        }

        if (!abi_details_success) {
            loader_free(field_core_sizes);
            loader_free(field_core_alignments);
            return false; // error_buf set by get_component_type_core_abi_details
        }
        total_wasm_alloc_size = align_up(total_wasm_alloc_size, max_alignment);

        // 2. Allocate memory in Wasm
        uint32 wasm_record_offset = 0;
        void *wasm_record_native_ptr = NULL;
        if (total_wasm_alloc_size > 0) { // Might be 0 for record with all empty fields
            wasm_record_native_ptr = wasm_runtime_module_malloc(module_inst, total_wasm_alloc_size, (void**)&wasm_record_offset);
            if (!wasm_record_native_ptr) {
                set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for record.", total_wasm_alloc_size);
                loader_free(field_core_sizes);
                loader_free(field_core_alignments);
                return false;
            }
        } else {
             // If total_wasm_alloc_size is 0 (e.g. record of empty records/tuples),
             // wasm_record_offset can remain 0 or point to a designated static empty region if ABI requires.
             // For now, use 0.
        }


        // 3. Lower each field into the allocated Wasm memory
        uint32 current_offset_in_wasm_struct = 0;
        for (uint32 i = 0; i < record_type->field_count; ++i) {
            void *host_field_ptr = component_fields_ptrs[i];
            WASMComponentValType *field_val_type = record_type->fields[i].valtype;
            uint8 field_target_core_type = VALUE_TYPE_VOID; // Type of data being written into Wasm memory for this field

            if (field_val_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
                field_target_core_type = get_core_wasm_type_for_primitive(field_val_type->u.primitive);
            } else if (field_val_type->kind == VAL_TYPE_KIND_LIST || 
                       field_val_type->kind == VAL_TYPE_KIND_STRING ||
                       field_val_type->kind == VAL_TYPE_KIND_RECORD ||
                       field_val_type->kind == VAL_TYPE_KIND_TUPLE ||
                       field_val_type->kind == VAL_TYPE_KIND_VARIANT || // Assuming these lower to i32 offset/value
                       field_val_type->kind == VAL_TYPE_KIND_OPTION  ||
                       field_val_type->kind == VAL_TYPE_KIND_RESULT  ||
                       field_val_type->kind == VAL_TYPE_KIND_ENUM    ||
                       field_val_type->kind == VAL_TYPE_KIND_FLAGS   ||
                       field_val_type->kind == VAL_TYPE_KIND_OWN     ||
                       field_val_type->kind == VAL_TYPE_KIND_BORROW) {
                field_target_core_type = VALUE_TYPE_I32; // These lower to an offset or (offset,len) pair or handle
            } else {
                set_canon_error_v(error_buf, error_buf_size, "Unhandled record field type %d for lowering field %u.", field_val_type->kind, i);
                if (wasm_record_native_ptr) wasm_runtime_module_free(module_inst, wasm_record_offset);
                loader_free(field_core_sizes);
                loader_free(field_core_alignments);
                return false;
            }

            current_offset_in_wasm_struct = align_up(current_offset_in_wasm_struct, field_core_alignments[i]);
            void *core_field_write_ptr_in_wasm = (uint8*)wasm_record_native_ptr + current_offset_in_wasm_struct;

            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_field_ptr, field_val_type,
                                                  field_target_core_type,
                                                  core_field_write_ptr_in_wasm, 
                                                  error_buf, error_buf_size)) {
                if (wasm_record_native_ptr && total_wasm_alloc_size > 0) wasm_runtime_module_free(module_inst, wasm_record_offset);
                // Error message is set by recursive call, or we can set a more specific one here.
                // For now, rely on recursive error.
                loader_free(field_core_sizes);
                loader_free(field_core_alignments);
                return false;
            }
            current_offset_in_wasm_struct += field_core_sizes[i];
        }
        
        loader_free(field_core_sizes);
        loader_free(field_core_alignments);

        // 4. Write the Wasm offset of the record structure to core_value_write_ptr
        if (target_core_wasm_type == VALUE_TYPE_I32) {
            *(uint32*)core_value_write_ptr = wasm_record_offset;
        } else {
            set_canon_error_v(error_buf, error_buf_size, "Record lowering expects target core type I32 for offset, got %u", target_core_wasm_type);
            if (wasm_record_native_ptr) wasm_runtime_module_free(module_inst, wasm_record_offset);
            return false;
        }
        
        LOG_VERBOSE("Lowered record to wasm mem offset %u, total_size %u", wasm_record_offset, total_wasm_alloc_size);
        return true;
    } else if (source_component_valtype->kind == VAL_TYPE_KIND_ENUM) {
        // Enums are represented as i32 discriminants on the host side (component_value_ptr points to uint32).
        // Target core Wasm type should be I32.
        if (target_core_wasm_type != VALUE_TYPE_I32) {
            set_canon_error_v(error_buf, error_buf_size, "Enum lowering expects target core type I32, got %u", target_core_wasm_type);
            return false;
        }
        if (!component_value_ptr) {
            set_canon_error(error_buf, error_buf_size, "Enum lowering received null component_value_ptr.");
            return false;
        }
        *(uint32*)core_value_write_ptr = *(uint32*)component_value_ptr;
        return true;
    } else if (source_component_valtype->kind == VAL_TYPE_KIND_OPTION) {
        // Host representation: WAMRHostGeneralValue { uint32_t disc; void* val; }
        // component_value_ptr points to this host struct.
        // Target core Wasm type should be I32 (offset to the allocated option structure).
        if (target_core_wasm_type != VALUE_TYPE_I32) {
            set_canon_error_v(error_buf, error_buf_size, "Option lowering expects target core type I32 for offset, got %u", target_core_wasm_type);
            return false;
        }
        if (!module_inst) {
             set_canon_error(error_buf, error_buf_size, "Module instance required for option lowering.");
             return false;
        }
         if (mem_idx == (uint32)-1 && canonical_def) {
            set_canon_error(error_buf, error_buf_size, "Option lowering requires memory option if using canonical_def.");
            return false;
        }

        WAMRHostGeneralValue *host_option = (WAMRHostGeneralValue*)component_value_ptr;
        if (!host_option) {
            set_canon_error(error_buf, error_buf_size, "Null host option value for lowering.");
            return false;
        }

        uint32 wasm_option_total_size, wasm_option_total_align;
        if (!get_component_type_core_abi_details(source_component_valtype, module_inst, 
                                                &wasm_option_total_size, &wasm_option_total_align,
                                                error_buf, error_buf_size)) {
            return false; // Error set by helper
        }

        uint32 wasm_option_offset = 0;
        uint8 *wasm_option_native_ptr = NULL;

        if (wasm_option_total_size > 0) {
            wasm_option_native_ptr = wasm_runtime_module_malloc(module_inst, wasm_option_total_size, (void**)&wasm_option_offset);
            if (!wasm_option_native_ptr) {
                set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for option.", wasm_option_total_size);
                return false;
            }
        } else { 
            // For an option that results in zero size (e.g. option<empty_type> where empty_type is zero sized)
            // The offset can be 0 or a canonical non-null if required by ABI. Assuming 0 for now.
        }
        
        // Write discriminant
        *(uint32*)wasm_option_native_ptr = host_option->disc;

        if (host_option->disc == 1) { // is_some, lower payload
            if (!host_option->val) {
                 set_canon_error(error_buf, error_buf_size, "Host option is 'some' but payload value is null.");
                 if (wasm_option_native_ptr) wasm_runtime_module_free(module_inst, wasm_option_offset);
                 return false;
            }
            WASMComponentOptionType *option_type = &source_component_valtype->u.option;
            WASMComponentValType *payload_valtype = option_type->valtype;

            uint32 payload_wasm_size, payload_wasm_align; // Size/align of the payload type itself
            if (!get_component_type_core_abi_details(payload_valtype, module_inst,
                                                    &payload_wasm_size, &payload_wasm_align,
                                                    error_buf, error_buf_size)) {
                if (wasm_option_native_ptr) wasm_runtime_module_free(module_inst, wasm_option_offset);
                return false; // Error set by helper
            }
            
            uint32 discriminant_size = sizeof(uint32);
            uint32 payload_offset_in_wasm_struct = align_up(discriminant_size, payload_wasm_align);
            void *payload_write_addr = wasm_option_native_ptr + payload_offset_in_wasm_struct;

            uint8 payload_target_core_type = VALUE_TYPE_I32; // Default for complex types (offset)
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                payload_target_core_type = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }
            
            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_option->val, payload_valtype,
                                                  payload_target_core_type,
                                                  payload_write_addr,
                                                  error_buf, error_buf_size)) {
                if (wasm_option_native_ptr && wasm_option_total_size > 0) wasm_runtime_module_free(module_inst, wasm_option_offset);
                // Error message is set by recursive call.
                return false;
            }
        } else if (host_option->disc != 0) { // Invalid discriminant
             set_canon_error_v(error_buf, error_buf_size, "Invalid discriminant %u for host option value", host_option->disc);
             if (wasm_option_native_ptr && wasm_option_total_size > 0) wasm_runtime_module_free(module_inst, wasm_option_offset);
             return false;
        }

        *(uint32*)core_value_write_ptr = wasm_option_offset;
        return true;

    } else if (source_component_valtype->kind == VAL_TYPE_KIND_RESULT) {
        // Host: WAMRHostGeneralValue { uint32_t disc (0=ok, 1=err); void* val; }
        // component_value_ptr points to this host struct.
        // Target core Wasm: i32 for offset of the allocated result structure.
        if (target_core_wasm_type != VALUE_TYPE_I32) {
            set_canon_error_v(error_buf, error_buf_size, "Result lowering expects target core type I32 for offset, got %u", target_core_wasm_type);
            return false;
        }
        if (!module_inst) {
             set_canon_error(error_buf, error_buf_size, "Module instance required for result lowering.");
             return false;
        }
        if (mem_idx == (uint32)-1 && canonical_def) {
            set_canon_error(error_buf, error_buf_size, "Result lowering requires memory option.");
            return false;
        }

        WAMRHostGeneralValue *host_result = (WAMRHostGeneralValue*)component_value_ptr;
        if (!host_result) {
            set_canon_error(error_buf, error_buf_size, "Null host result value for lowering.");
            return false;
        }

        uint32 wasm_result_total_size, wasm_result_total_align;
        if (!get_component_type_core_abi_details(source_component_valtype, module_inst, 
                                                &wasm_result_total_size, &wasm_result_total_align,
                                                error_buf, error_buf_size)) {
            return false; // Error set by helper
        }

        uint32 wasm_result_offset = 0;
        uint8 *wasm_result_native_ptr = NULL;

        if (wasm_result_total_size > 0) {
            wasm_result_native_ptr = wasm_runtime_module_malloc(module_inst, wasm_result_total_size, (void**)&wasm_result_offset);
            if (!wasm_result_native_ptr) {
                set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for result.", wasm_result_total_size);
                return false;
            }
        }

        // Write discriminant
        *(uint32*)wasm_result_native_ptr = host_result->disc;

        WASMComponentResultType *result_type = &source_component_valtype->u.result;
        WASMComponentValType *payload_valtype = NULL;

        if (host_result->disc == 0) { // ok
            payload_valtype = result_type->ok_valtype;
        } else if (host_result->disc == 1) { // err
            payload_valtype = result_type->err_valtype;
        } else { // Invalid discriminant
            set_canon_error_v(error_buf, error_buf_size, "Invalid discriminant %u for host result value", host_result->disc);
            if (wasm_result_native_ptr) wasm_runtime_module_free(module_inst, wasm_result_offset);
            return false;
        }

        if (payload_valtype && host_result->val) { // If there's a payload type for this case AND host has a value
            uint32 ok_s, ok_a, err_s, err_a; // For calculating max_payload_align for offset
            ok_a = err_a = 1; 
            if(result_type->ok_valtype) get_component_type_core_abi_details(result_type->ok_valtype, module_inst, &ok_s, &ok_a, error_buf, error_buf_size);
            if(result_type->err_valtype) get_component_type_core_abi_details(result_type->err_valtype, module_inst, &err_s, &err_a, error_buf, error_buf_size);
            uint32 max_payload_align_for_calc = ok_a > err_a ? ok_a : err_a;
            if (max_payload_align_for_calc == 0) max_payload_align_for_calc = 1;

            uint32 discriminant_size = sizeof(uint32);
            uint32 payload_offset_in_wasm_struct = align_up(discriminant_size, max_payload_align_for_calc);
            void *payload_write_addr = wasm_result_native_ptr + payload_offset_in_wasm_struct;

            uint8 payload_target_core_type = VALUE_TYPE_I32;
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                payload_target_core_type = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }
            
            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_result->val, payload_valtype,
                                                  payload_target_core_type,
                                                  payload_write_addr,
                                                  error_buf, error_buf_size)) {
                if (wasm_result_native_ptr && wasm_result_total_size > 0) wasm_runtime_module_free(module_inst, wasm_result_offset);
                // Error message propagated from recursive call
                return false; 
            }
        } else if (payload_valtype && !host_result->val) {
            // This case means the schema expects a payload but the host provided NULL.
            // Depending on strictness, this could be an error. For now, assume it means the payload is "empty" or default.
            // The Wasm memory for the payload area is allocated but remains uninitialized or zeroed by malloc.
            LOG_VERBOSE("Host result has payload type but null value pointer for disc %u.", host_result->disc);
        }


        *(uint32*)core_value_write_ptr = wasm_result_offset;
        return true;

    } else if (source_component_valtype->kind == VAL_TYPE_KIND_VARIANT) {
        // Host: WAMRHostGeneralValue { uint32_t disc (case_index); void* val; }
        // Target core Wasm: i32 for offset of the allocated variant structure.
        if (target_core_wasm_type != VALUE_TYPE_I32) {
            set_canon_error_v(error_buf, error_buf_size, "Variant lowering expects target core type I32 for offset, got %u", target_core_wasm_type);
            return false;
        }
        if (!module_inst) {
             set_canon_error(error_buf, error_buf_size, "Module instance required for variant lowering.");
             return false;
        }
        if (mem_idx == (uint32)-1 && canonical_def) {
            set_canon_error(error_buf, error_buf_size, "Variant lowering requires memory option.");
            return false;
        }

        WAMRHostGeneralValue *host_variant = (WAMRHostGeneralValue*)component_value_ptr;
        if (!host_variant) {
            set_canon_error(error_buf, error_buf_size, "Null host variant value for lowering.");
            return false;
        }
        
        WASMComponentVariantType *variant_type = &source_component_valtype->u.variant;
        if (host_variant->disc >= variant_type->case_count) {
            set_canon_error_v(error_buf, error_buf_size, "Invalid discriminant %u for host variant with %u cases", host_variant->disc, variant_type->case_count);
            return false;
        }

        uint32 wasm_variant_total_size, wasm_variant_total_align;
        if (!get_component_type_core_abi_details(source_component_valtype, module_inst, 
                                                &wasm_variant_total_size, &wasm_variant_total_align,
                                                error_buf, error_buf_size)) {
            return false; // Error set by helper
        }

        uint32 wasm_variant_offset = 0;
        uint8 *wasm_variant_native_ptr = NULL;

        if (wasm_variant_total_size > 0) {
            wasm_variant_native_ptr = wasm_runtime_module_malloc(module_inst, wasm_variant_total_size, (void**)&wasm_variant_offset);
            if (!wasm_variant_native_ptr) {
                set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for variant.", wasm_variant_total_size);
                return false;
            }
        }
        
        // Write discriminant
        *(uint32*)wasm_variant_native_ptr = host_variant->disc;

        WASMComponentCase *active_case = &variant_type->cases[host_variant->disc];
        WASMComponentValType *payload_valtype = active_case->valtype;

        if (payload_valtype && host_variant->val) {
            uint32 max_case_payload_align = 1;
            // Recalculate max_case_payload_align for offset calculation
            for (uint32 i = 0; i < variant_type->case_count; ++i) {
                if (variant_type->cases[i].valtype) {
                    uint32 case_s, case_a;
                    // Silently ignore error here, main check is for active_case->valtype
                    if (get_component_type_core_abi_details(variant_type->cases[i].valtype, module_inst, &case_s, &case_a, NULL, 0)) {
                         if (case_a > max_case_payload_align) max_case_payload_align = case_a;
                    }
                }
            }
            if (max_case_payload_align == 0) max_case_payload_align = 1;

            uint32 discriminant_size = sizeof(uint32);
            uint32 payload_offset_in_wasm_struct = align_up(discriminant_size, max_case_payload_align);
            void *payload_write_addr = wasm_variant_native_ptr + payload_offset_in_wasm_struct;

            uint8 payload_target_core_type = VALUE_TYPE_I32;
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                payload_target_core_type = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }
            
            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_variant->val, payload_valtype,
                                                  payload_target_core_type,
                                                  payload_write_addr,
                                                  error_buf, error_buf_size)) {
                if (wasm_variant_native_ptr && wasm_variant_total_size > 0) wasm_runtime_module_free(module_inst, wasm_variant_offset);
                // Error propagated from recursive call
                return false; 
            }
        } else if (payload_valtype && !host_variant->val) {
            LOG_VERBOSE("Host variant has payload type but null value pointer for disc %u.", host_variant->disc);
        }

        *(uint32*)core_value_write_ptr = wasm_variant_offset;
        return true;

    } else if (source_component_valtype->kind == VAL_TYPE_KIND_OWN_TYPE_IDX ||
               source_component_valtype->kind == VAL_TYPE_KIND_BORROW_TYPE_IDX) {
        // Resource handles are i32 on the host side (component_value_ptr points to uint32).
        // Target core Wasm type should be I32.
        if (target_core_wasm_type != VALUE_TYPE_I32) {
            set_canon_error_v(error_buf, error_buf_size, "Resource handle lowering expects target core type I32, got %u", target_core_wasm_type);
            return false;
        }
        if (!component_value_ptr) {
            set_canon_error(error_buf, error_buf_size, "Resource handle lowering received null component_value_ptr.");
            return false;
        }
        
        uint32_t host_handle = *(uint32_t*)component_value_ptr;

        // Optional: Validate if the handle is active for 'own' types before lowering.
        // This implies the host is giving up ownership or asserting its validity.
        // if (source_component_valtype->kind == VAL_TYPE_KIND_OWN_TYPE_IDX) {
        //    initialize_resource_table(); // Ensure table is ready
        //    if (host_handle == 0 || host_handle >= MAX_RESOURCE_HANDLES || !global_resource_table[host_handle].is_active) {
        //        set_canon_error_v(error_buf, error_buf_size, "Attempting to lower an inactive/invalid OWN resource handle %u.", host_handle);
        //        return false;
        //    }
        // }
        // For BORROW, the handle might be one Wasm previously gave to the host, so it might not be in the host's ownership table.
        // For now, pass through the integer value directly. Wasm module is responsible for its validity.

        *(uint32_t*)core_value_write_ptr = host_handle;
        return true;
    } else if (source_component_valtype->kind == VAL_TYPE_KIND_TUPLE) {
        WASMComponentTupleType *tuple_type = &source_component_valtype->u.tuple;
        // component_value_ptr is void** (array of pointers to component element values)
        void **component_elements_ptrs = (void**)component_value_ptr;

        if (!module_inst) {
             set_canon_error(error_buf, error_buf_size, "Module instance required for tuple lowering to memory.");
             return false;
        }
        if (mem_idx == (uint32)-1 && canonical_def) {
            set_canon_error(error_buf, error_buf_size, "Tuple lowering requires memory option if using canonical_def.");
            return false;
        }

        // 1. Calculate total size and alignment requirements for the flat Wasm memory structure
        uint32 total_wasm_alloc_size = 0;
        uint32 max_alignment = 1; 
        // Temp storage for sizes and alignments to avoid recalculating
        uint32 *element_core_sizes = NULL;
        uint32 *element_core_alignments = NULL;
        bool abi_details_success = true;

        if (tuple_type->field_count > 0) {
            element_core_sizes = loader_malloc(tuple_type->field_count * sizeof(uint32), error_buf, error_buf_size);
            if (!element_core_sizes) { 
                set_canon_error(error_buf, error_buf_size, "Failed to allocate memory for element sizes.");
                return false; 
            }
            element_core_alignments = loader_malloc(tuple_type->field_count * sizeof(uint32), error_buf, error_buf_size);
            if (!element_core_alignments) { 
                loader_free(element_core_sizes); 
                set_canon_error(error_buf, error_buf_size, "Failed to allocate memory for element alignments.");
                return false; 
            }
        }

        for (uint32 i = 0; i < tuple_type->field_count; ++i) {
            WASMComponentValType *element_val_type = tuple_type->fields[i].valtype;
            if (!get_component_type_core_abi_details(element_val_type, module_inst,
                                                    &element_core_sizes[i], &element_core_alignments[i],
                                                    error_buf, error_buf_size)) {
                abi_details_success = false;
                break;
            }
            if (element_core_alignments[i] > max_alignment) {
                max_alignment = element_core_alignments[i];
            }
            total_wasm_alloc_size = align_up(total_wasm_alloc_size, element_core_alignments[i]);
            total_wasm_alloc_size += element_core_sizes[i];
        }

        if (!abi_details_success) {
            if(element_core_sizes) loader_free(element_core_sizes);
            if(element_core_alignments) loader_free(element_core_alignments);
            // error_buf should be set by get_component_type_core_abi_details
            return false;
        }
        
        total_wasm_alloc_size = align_up(total_wasm_alloc_size, max_alignment); // Align total struct size

        // 2. Allocate memory in Wasm
        uint32 wasm_tuple_offset = 0;
        void *wasm_tuple_native_ptr = NULL;
        if (total_wasm_alloc_size > 0) {
            wasm_tuple_native_ptr = wasm_runtime_module_malloc(module_inst, total_wasm_alloc_size, (void**)&wasm_tuple_offset);
            if (!wasm_tuple_native_ptr) {
                set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for tuple.", total_wasm_alloc_size);
                if(element_core_sizes) loader_free(element_core_sizes);
                if(element_core_alignments) loader_free(element_core_alignments);
                return false;
            }
        }
        
        // 3. Lower each element into the allocated Wasm memory
        uint32 current_offset_in_wasm_struct = 0;
        for (uint32 i = 0; i < tuple_type->field_count; ++i) {
            void *host_elem_ptr = component_elements_ptrs[i];
            WASMComponentValType *element_val_type = tuple_type->fields[i].valtype;
            uint8 element_target_core_type = VALUE_TYPE_VOID; // The type of the data being written into wasm memory for this element

            // Determine the target core type for the recursive call (type of the data in wasm memory)
             if (element_val_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
                element_target_core_type = get_core_wasm_type_for_primitive(element_val_type->u.primitive);
            } else if (element_val_type->kind == VAL_TYPE_KIND_LIST || 
                       element_val_type->kind == VAL_TYPE_KIND_STRING ||
                       element_val_type->kind == VAL_TYPE_KIND_RECORD ||
                       element_val_type->kind == VAL_TYPE_KIND_TUPLE) {
                // These are lowered to offsets or (offset, len) pairs, which are i32s.
                element_target_core_type = VALUE_TYPE_I32; 
            } else {
                 // Should have been caught by size calculation
                set_canon_error_v(error_buf, error_buf_size, "Unhandled tuple element type %d for lowering.", i);
                // Note: wasm_runtime_module_free is not called here as it might be complex if partially filled.
                // The caller should handle this transactionally or by other means if an error occurs mid-way.
                if(element_core_sizes) loader_free(element_core_sizes);
                if(element_core_alignments) loader_free(element_core_alignments);
                // No easy way to free wasm_tuple_native_ptr here if some elements are already written.
                return false;
            }


            current_offset_in_wasm_struct = align_up(current_offset_in_wasm_struct, element_core_alignments[i]);
            void *core_elem_write_ptr_in_wasm = (uint8*)wasm_tuple_native_ptr + current_offset_in_wasm_struct;

            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_elem_ptr, element_val_type,
                                                  element_target_core_type, // This is the type being written to Wasm memory
                                                  core_elem_write_ptr_in_wasm, 
                                                  error_buf, error_buf_size)) {
                // Error, potentially free wasm_tuple_offset if allocated
                if (wasm_tuple_native_ptr && total_wasm_alloc_size > 0) wasm_runtime_module_free(module_inst, wasm_tuple_offset);
                // Error message is set by recursive call or we can set a more specific one.
                // For now, rely on recursive error.
                if(element_core_sizes) loader_free(element_core_sizes);
                if(element_core_alignments) loader_free(element_core_alignments);
                return false;
            }
            current_offset_in_wasm_struct += element_core_sizes[i];
        }
        
        if(element_core_sizes) loader_free(element_core_sizes);
        if(element_core_alignments) loader_free(element_core_alignments);

        // 4. Write the Wasm offset of the tuple structure to core_value_write_ptr
        // target_core_wasm_type for the tuple itself should be I32 (offset)
        if (target_core_wasm_type == VALUE_TYPE_I32) {
            *(uint32*)core_value_write_ptr = wasm_tuple_offset;
        } else {
            // This case might occur if a tuple is returned directly in a way not via pointer,
            // which is unlikely for non-trivial tuples with the component model ABI.
            // Or if the target expects e.g. i64 for the pointer.
            set_canon_error_v(error_buf, error_buf_size, "Tuple lowering expects target core type I32 for offset, got %u", target_core_wasm_type);
            if (wasm_tuple_native_ptr) wasm_runtime_module_free(module_inst, wasm_tuple_offset);
            return false;
        }
        
        LOG_VERBOSE("Lowered tuple to wasm mem offset %u, total_size %u", wasm_tuple_offset, total_wasm_alloc_size);
        return true;
    }

    set_canon_error_v(error_buf, error_buf_size, "Unsupported type kind for lowering: %d", source_component_valtype->kind);
    return false;
}

static uint32 align_up(uint32 val, uint32 alignment) {
    if (alignment == 0) {
        // This case should ideally not be reached if types are well-defined.
        // Consider logging an error or asserting. For now, return val to avoid division by zero.
        LOG_ERROR("Align_up called with alignment 0");
        return val; 
    }
    return (val + alignment - 1) & ~(alignment - 1);
}

// Helper function to get size and alignment for a component type when laid out in core Wasm flat memory
static bool
get_component_type_core_abi_details(const WASMComponentValType *val_type,
                                    WASMModuleInstance *module_inst, // May be needed for context (e.g. type definitions from module)
                                    uint32 *out_size, uint32 *out_alignment,
                                    char* error_buf, uint32 error_buf_size)
{
    if (!val_type || !out_size || !out_alignment) {
        set_canon_error(error_buf, error_buf_size, "Invalid arguments for get_component_type_core_abi_details.");
        return false;
    }

    switch (val_type->kind) {
        case VAL_TYPE_KIND_PRIMITIVE:
        {
            uint8 core_type = get_core_wasm_type_for_primitive(val_type->u.primitive);
            *out_size = get_core_wasm_primitive_size(core_type);
            if (*out_size == 0 && val_type->u.primitive != PRIM_VAL_UNDEFINED) { // Undefined might be zero size, but other primitives shouldn't.
                set_canon_error_v(error_buf, error_buf_size, "Unsupported primitive type or zero size for ABI details: %d", val_type->u.primitive);
                return false;
            }
            *out_alignment = *out_size; 
            if (*out_alignment == 0 && val_type->u.primitive == PRIM_VAL_UNDEFINED) { // Handle Undefined case for alignment
                *out_alignment = 1; // Treat as alignment 1 if size is 0
            }
            return true;
        }
        case VAL_TYPE_KIND_STRING: // Stores (ptr: i32, len: i32)
        case VAL_TYPE_KIND_LIST:   // Stores (ptr: i32, len: i32)
            *out_size = 2 * sizeof(uint32); 
            *out_alignment = sizeof(uint32); 
            return true;
        case VAL_TYPE_KIND_TUPLE:
        {
            WASMComponentTupleType *tuple_type = &val_type->u.tuple;
            uint32 current_offset = 0;
            uint32 max_align = 1;
            if (tuple_type->field_count == 0) { // Empty tuple
                *out_size = 0;
                *out_alignment = 1;
                return true;
            }
            for (uint32 i = 0; i < tuple_type->field_count; ++i) {
                uint32 field_size, field_alignment;
                if (!get_component_type_core_abi_details(tuple_type->fields[i].valtype, module_inst, &field_size, &field_alignment, error_buf, error_buf_size)) {
                    return false; // Error already set
                }
                current_offset = align_up(current_offset, field_alignment);
                current_offset += field_size;
                if (field_alignment > max_align) {
                    max_align = field_alignment;
                }
            }
            *out_size = align_up(current_offset, max_align);
            *out_alignment = max_align;
            return true;
        }
        case VAL_TYPE_KIND_RECORD:
        {
            WASMComponentRecordType *record_type = &val_type->u.record;
            uint32 current_offset = 0;
            uint32 max_align = 1;
            if (record_type->field_count == 0) { // Empty record
                *out_size = 0;
                *out_alignment = 1;
                return true;
            }
            for (uint32 i = 0; i < record_type->field_count; ++i) {
                uint32 field_size, field_alignment;
                if (!get_component_type_core_abi_details(record_type->fields[i].valtype, module_inst, &field_size, &field_alignment, error_buf, error_buf_size)) {
                    return false; // Error already set
                }
                current_offset = align_up(current_offset, field_alignment);
                current_offset += field_size;
                if (field_alignment > max_align) {
                    max_align = field_alignment;
                }
            }
            *out_size = align_up(current_offset, max_align);
            *out_alignment = max_align;
            return true;
        }
        case VAL_TYPE_KIND_ENUM:
            *out_size = sizeof(uint32); // Enums are discriminants, typically i32
            *out_alignment = sizeof(uint32);
            return true;
        case VAL_TYPE_KIND_OPTION:
        {
            WASMComponentOptionType *option_type = &val_type->u.option;
            uint32 disc_size = sizeof(uint32); // Discriminant for some/none
            uint32 disc_align = sizeof(uint32);
            
            uint32 val_size = 0;
            uint32 val_align = 1;

            // Get details for the value type if it's 'some'
            if (!get_component_type_core_abi_details(option_type->valtype, module_inst, &val_size, &val_align, error_buf, error_buf_size)) {
                return false; // Error set by nested call
            }

            // Option layout: discriminant, then potentially payload
            // Payload is only present for 'some', but space might be reserved or optimized.
            // Canonical ABI: discriminant followed by value, value is aligned.
            uint32 payload_offset = align_up(disc_size, val_align);
            *out_size = payload_offset + val_size;
            *out_alignment = disc_align > val_align ? disc_align : val_align; // Max of discriminant and value alignment
            // The overall size must be aligned to its own alignment requirement
            *out_size = align_up(*out_size, *out_alignment);
            return true;
        }
        case VAL_TYPE_KIND_RESULT:
        {
            WASMComponentResultType *result_type = &val_type->u.result;
            uint32 disc_size = sizeof(uint32); // Discriminant for ok/err
            uint32 disc_align = sizeof(uint32);

            uint32 ok_size = 0, ok_align = 1;
            uint32 err_size = 0, err_align = 1;
            uint32 max_payload_size = 0;
            uint32 max_payload_align = 1;

            if (result_type->ok_valtype) { // ok_valtype can be NULL for `result` or `result<_, >`
                if (!get_component_type_core_abi_details(result_type->ok_valtype, module_inst, &ok_size, &ok_align, error_buf, error_buf_size)) {
                    return false;
                }
            }
            if (result_type->err_valtype) { // err_valtype can be NULL for `result<_>` or `result<_, >`
                 if (!get_component_type_core_abi_details(result_type->err_valtype, module_inst, &err_size, &err_align, error_buf, error_buf_size)) {
                    return false;
                }
            }
            
            max_payload_size = ok_size > err_size ? ok_size : err_size;
            max_payload_align = ok_align > err_align ? ok_align : err_align;
            
            // Result layout: discriminant, then payload area (for ok or err)
            uint32 payload_offset = align_up(disc_size, max_payload_align);
            *out_size = payload_offset + max_payload_size;
            *out_alignment = disc_align > max_payload_align ? disc_align : max_payload_align;
            *out_size = align_up(*out_size, *out_alignment);
            return true;
        }
        case VAL_TYPE_KIND_VARIANT:
        {
            WASMComponentVariantType *variant_type = &val_type->u.variant;
            uint32 disc_size = sizeof(uint32); // Discriminant size (assuming i32 for now)
            uint32 disc_align = sizeof(uint32);
            
            uint32 max_case_payload_size = 0;
            uint32 max_case_payload_align = 1;

            if (variant_type->case_count == 0) { // Should not happen for valid variants
                *out_size = disc_size;
                *out_alignment = disc_align;
                return true;
            }

            for (uint32 i = 0; i < variant_type->case_count; ++i) {
                WASMComponentCase *current_case = &variant_type->cases[i];
                if (current_case->valtype) { // Case has a payload
                    uint32 case_payload_size, case_payload_align;
                    if (!get_component_type_core_abi_details(current_case->valtype, module_inst, &case_payload_size, &case_payload_align, error_buf, error_buf_size)) {
                        return false; // Error set
                    }
                    if (case_payload_size > max_case_payload_size) {
                        max_case_payload_size = case_payload_size;
                    }
                    if (case_payload_align > max_case_payload_align) {
                        max_case_payload_align = case_payload_align;
                    }
                }
            }
            
            // Variant layout: discriminant, then payload area (for the largest case)
            uint32 payload_offset = align_up(disc_size, max_case_payload_align);
            *out_size = payload_offset + max_case_payload_size;
            *out_alignment = disc_align > max_case_payload_align ? disc_align : max_case_payload_align;
            *out_size = align_up(*out_size, *out_alignment);
            return true;
        }
        case VAL_TYPE_KIND_FLAGS:   // Size depends on number of flags
            // Number of i32s needed to store flags. E.g. up to 32 flags = 1 i32, up to 64 flags = 2 i32s.
            if (val_type->u.flags.label_count == 0) { // No flags defined (unlikely for useful type)
                *out_size = 0;
                *out_alignment = 1;
            } else if (val_type->u.flags.label_count <= 32) {
                *out_size = sizeof(uint32);
                *out_alignment = sizeof(uint32);
            } else if (val_type->u.flags.label_count <= 64) {
                *out_size = 2 * sizeof(uint32); // Represented as two i32s
                *out_alignment = sizeof(uint32); // Alignment of i32
            } else {
                // Canonical ABI supports more, but for now, let's cap or use multiple i32s.
                // For simplicity, let's estimate with multiple i32s.
                uint32 num_u32s = (val_type->u.flags.label_count + 31) / 32;
                *out_size = num_u32s * sizeof(uint32);
                *out_alignment = sizeof(uint32);
            }
            return true; 

        // Resources are handles (i32)
        case VAL_TYPE_KIND_OWN:
        case VAL_TYPE_KIND_BORROW:
            *out_size = sizeof(int32_t);
            *out_alignment = sizeof(int32_t);
            return true;

        default:
            set_canon_error_v(error_buf, error_buf_size, "Unsupported type kind for ABI details: %d", val_type->kind);
            *out_size = 0;
            *out_alignment = 1; // Minimum alignment
            return false;
    }
}


bool
wasm_component_canon_resource_new(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def,
    void *core_value_write_ptr,
    char *error_buf, uint32 error_buf_size)
{
    initialize_resource_table();
    uint32_t new_handle = (uint32_t)-1;

    for (uint32_t i = 1; i < MAX_RESOURCE_HANDLES; ++i) {
        uint32_t current_idx = (next_resource_handle + i -1) % (MAX_RESOURCE_HANDLES -1) + 1;
        if (current_idx == 0) current_idx = 1; // Ensure handle 0 is not used by wrap around
        if (!global_resource_table[current_idx].is_active) {
            new_handle = current_idx;
            break;
        }
    }
    
    if (new_handle == (uint32_t)-1) { 
         for (uint32_t i = 1; i < MAX_RESOURCE_HANDLES; ++i) { 
            if (!global_resource_table[i].is_active) {
                new_handle = i;
                break;
            }
        }
    }

    if (new_handle == (uint32_t)-1) {
        set_canon_error(error_buf, error_buf_size, "Resource table full.");
        return false;
    }
    
    next_resource_handle = new_handle + 1; 
    if (next_resource_handle >= MAX_RESOURCE_HANDLES) {
        next_resource_handle = 1; 
    }

    global_resource_table[new_handle].is_active = true;
    global_resource_table[new_handle].component_resource_type_idx = canonical_def->u.type_idx_op.type_idx;
    global_resource_table[new_handle].host_data = NULL; 

    *(int32_t*)core_value_write_ptr = (int32_t)new_handle;
    LOG_VERBOSE("Created new resource handle %d for component type idx %u", new_handle, global_resource_table[new_handle].component_resource_type_idx);
    return true;
}

bool
wasm_component_canon_resource_drop(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def,
    void *component_handle_ptr, 
    char *error_buf, uint32 error_buf_size)
{
    initialize_resource_table();
    int32_t handle = *(int32_t*)component_handle_ptr;

    if (handle <= 0 || handle >= MAX_RESOURCE_HANDLES) { 
        set_canon_error_v(error_buf, error_buf_size, "Invalid resource handle %d for drop.", handle);
        return false;
    }
    if (!global_resource_table[handle].is_active) {
        set_canon_error_v(error_buf, error_buf_size, "Resource handle %d already inactive for drop.", handle);
        return false; 
    }
    
    global_resource_table[handle].is_active = false;
    global_resource_table[handle].host_data = NULL; 
    global_resource_table[handle].component_resource_type_idx = 0; 

    LOG_VERBOSE("Dropped resource handle %d", handle);
    return true;
}

bool
wasm_component_canon_resource_rep(
    WASMExecEnv *exec_env,
    const WASMComponentCanonical *canonical_def, 
    void *component_handle_ptr, 
    void *core_value_write_ptr, 
    char *error_buf, uint32 error_buf_size)
{
    initialize_resource_table();
    int32_t handle = *(int32_t*)component_handle_ptr;

    if (handle <= 0 || handle >= MAX_RESOURCE_HANDLES || !global_resource_table[handle].is_active) {
        set_canon_error_v(error_buf, error_buf_size, "Invalid or inactive resource handle %d for rep.", handle);
        return false;
    }

    *(int32_t*)core_value_write_ptr = handle;
    LOG_VERBOSE("Retrieved representation for resource handle %d (rep is %d)", handle, handle);
    return true;
}


static uint8 get_core_wasm_type_for_primitive(WASMComponentPrimValType prim_val_type) {
    switch (prim_val_type) {
        case PRIM_VAL_BOOL: return VALUE_TYPE_I32; // Bools are often i32 in core wasm
        case PRIM_VAL_S8:   return VALUE_TYPE_I32; // Promoted to i32
        case PRIM_VAL_U8:   return VALUE_TYPE_I32; // Promoted to i32
        case PRIM_VAL_S16:  return VALUE_TYPE_I32; // Promoted to i32
        case PRIM_VAL_U16:  return VALUE_TYPE_I32; // Promoted to i32
        case PRIM_VAL_S32:  return VALUE_TYPE_I32;
        case PRIM_VAL_U32:  return VALUE_TYPE_I32;
        case PRIM_VAL_S64:  return VALUE_TYPE_I64;
        case PRIM_VAL_U64:  return VALUE_TYPE_I64;
        case PRIM_VAL_F32:  return VALUE_TYPE_F32;
        case PRIM_VAL_F64:  return VALUE_TYPE_F64;
        case PRIM_VAL_CHAR: return VALUE_TYPE_I32; // Unicode char
        // String is not a single primitive passed by value, handled as (offset, len)
        default: return VALUE_TYPE_VOID; // Error/unknown
    }
}

static uint32 get_core_wasm_primitive_size(uint8 core_type_tag) {
    switch (core_type_tag) {
        case VALUE_TYPE_I32: return sizeof(uint32);
        case VALUE_TYPE_I64: return sizeof(uint64);
        case VALUE_TYPE_F32: return sizeof(float32);
        case VALUE_TYPE_F64: return sizeof(float64);
        default: return 0; 
    }
}
