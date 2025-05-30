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

        if (record_type->field_count > 0) {
            lifted_fields_array = loader_malloc(record_type->field_count * sizeof(void*), error_buf, error_buf_size);
            if (!lifted_fields_array) return false;
            memset(lifted_fields_array, 0, record_type->field_count * sizeof(void*));

            // Assumption: core_value_ptr is void**, pointing to an array of core field data pointers
            void **core_fields_ptrs = (void**)core_value_ptr;

            for (uint32 i = 0; i < record_type->field_count; ++i) {
                WASMComponentValType *field_val_type = record_type->fields[i].valtype;
                void *core_field_data_ptr = core_fields_ptrs[i];
                uint8 field_core_type_tag = VALUE_TYPE_VOID; // Placeholder

                if (field_val_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
                    field_core_type_tag = get_core_wasm_type_for_primitive(field_val_type->u.primitive);
                } else {
                    // This simplification needs to be addressed for nested records/lists etc.
                     set_canon_error_v(error_buf, error_buf_size, "Record lifting currently only supports primitive fields (field %u type %d).", i, field_val_type->kind);
                     // Cleanup already lifted fields
                    for (uint32 j = 0; j < i; ++j) if (lifted_fields_array[j]) loader_free(lifted_fields_array[j]);
                    loader_free(lifted_fields_array);
                    return false;
                }

                if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                    core_field_data_ptr, field_core_type_tag,
                                                    field_val_type,
                                                    &lifted_fields_array[i],
                                                    error_buf, error_buf_size)) {
                    for (uint32 j = 0; j < i; ++j) if (lifted_fields_array[j]) loader_free(lifted_fields_array[j]);
                    loader_free(lifted_fields_array);
                    return false;
                }
            }
        }
        *lifted_component_value_ptr = lifted_fields_array;
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
    uint32 mem_idx = (uint32)-1;

    if (!module_inst && source_component_valtype->kind != VAL_TYPE_KIND_PRIMITIVE) {
        set_canon_error(error_buf, error_buf_size, "Failed to get module instance from exec_env for lowering non-primitive.");
        return false;
    }
    
    if (canonical_def) { 
        for (uint32 i = 0; i < canonical_def->option_count; ++i) {
            if (canonical_def->options[i].kind == CANONICAL_OPTION_MEMORY_IDX) {
                mem_idx = canonical_def->options[i].value;
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
                void *alloc_native_ptr = wasm_runtime_module_malloc(module_inst, alloc_size, (void**)&wasm_offset); 
                
                if (!alloc_native_ptr) {
                    set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for string.", alloc_size);
                    return false;
                }
                bh_memcpy_s(alloc_native_ptr, alloc_size, str_to_lower, str_len);

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

        uint32 total_wasm_alloc_size = host_list->count * core_element_size;
        uint32 wasm_list_offset = 0;
        void *wasm_list_native_ptr = NULL;

        if (host_list->count > 0) { 
            wasm_list_native_ptr = wasm_runtime_module_malloc(module_inst, total_wasm_alloc_size, (void**)&wasm_list_offset);
            if (!wasm_list_native_ptr) {
                set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes in wasm memory for list.", total_wasm_alloc_size);
                return false;
            }
        }
        
        for (uint32 i = 0; i < host_list->count; ++i) {
            void *host_elem_ptr = host_list->elements[i];
            void *core_elem_write_ptr_in_wasm = (uint8*)wasm_list_native_ptr + (i * core_element_size);
            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_elem_ptr, element_valtype,
                                                  core_element_type_tag,
                                                  core_elem_write_ptr_in_wasm, 
                                                  error_buf, error_buf_size)) {
                if (wasm_list_native_ptr) wasm_runtime_module_free(module_inst, wasm_list_offset);
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
        // Assumption: component_value_ptr is void** (array of pointers to component field values)
        void **component_fields_ptrs = (void**)component_value_ptr;
        
        // core_value_write_ptr is assumed to be a void** that will point to an array of core data pointers
        // This means the caller needs to allocate space for this array of pointers if the core ABI is by reference.
        // If core ABI is by value (fields on stack), this model is insufficient.
        // For now, let's assume core_value_write_ptr is a pre-allocated buffer large enough to hold all lowered field values sequentially.
        // This is a major simplification and likely needs to change for a real ABI.
        // Let's revert to the idea that core_value_write_ptr is a void** to an array of pointers.
        
        void **lowered_fields_core_ptrs = NULL;
        if (record_type->field_count > 0) {
            lowered_fields_core_ptrs = loader_malloc(record_type->field_count * sizeof(void*), error_buf, error_buf_size);
            if (!lowered_fields_core_ptrs) return false;
            memset(lowered_fields_core_ptrs, 0, record_type->field_count * sizeof(void*));
        }

        for (uint32 i = 0; i < record_type->field_count; ++i) {
            WASMComponentValType *field_val_type = record_type->fields[i].valtype;
            void *component_field_ptr = component_fields_ptrs[i];
            uint8 field_core_type_tag = VALUE_TYPE_VOID;

            if (field_val_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
                field_core_type_tag = get_core_wasm_type_for_primitive(field_val_type->u.primitive);
            } else {
                set_canon_error_v(error_buf, error_buf_size, "Record lowering currently only supports primitive fields (field %u type %d).", i, field_val_type->kind);
                if (lowered_fields_core_ptrs) { // Free partially allocated field data if any
                    for(uint32 j=0; j<i; ++j) if(lowered_fields_core_ptrs[j]) loader_free(lowered_fields_core_ptrs[j]);
                    loader_free(lowered_fields_core_ptrs);
                }
                return false;
            }
            
            // Each field needs its own storage for the lowered value if it's not directly written
            // For primitives, we can assume they are written directly if core_value_write_ptr was a struct pointer.
            // Since we are creating an array of pointers, each primitive also needs its memory.
            uint32 core_field_size = get_core_wasm_primitive_size(field_core_type_tag);
            if (core_field_size == 0 && field_val_type->kind == VAL_TYPE_KIND_PRIMITIVE) { // Check if primitive but size 0 (error)
                 set_canon_error_v(error_buf, error_buf_size, "Cannot determine size for primitive field %u.", i);
                 if (lowered_fields_core_ptrs) {loader_free(lowered_fields_core_ptrs); } // Free array itself
                 return false;
            }
            lowered_fields_core_ptrs[i] = loader_malloc(core_field_size, error_buf, error_buf_size);
            if (!lowered_fields_core_ptrs[i]) {
                 for(uint32 j=0; j<i; ++j) if(lowered_fields_core_ptrs[j]) loader_free(lowered_fields_core_ptrs[j]);
                 if (lowered_fields_core_ptrs) {loader_free(lowered_fields_core_ptrs); }
                 return false;
            }

            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  component_field_ptr, field_val_type,
                                                  field_core_type_tag,
                                                  lowered_fields_core_ptrs[i], // Write to the allocated space for this field
                                                  error_buf, error_buf_size)) {
                for(uint32 j=0; j<=i; ++j) if(lowered_fields_core_ptrs[j]) loader_free(lowered_fields_core_ptrs[j]);
                if (lowered_fields_core_ptrs) {loader_free(lowered_fields_core_ptrs); }
                return false;
            }
        }
        // Make core_value_write_ptr (which is void*) point to our array of pointers (void**)
        *(void***)core_value_write_ptr = lowered_fields_core_ptrs; 
        return true;
    }


    set_canon_error_v(error_buf, error_buf_size, "Unsupported type kind for lowering: %d", source_component_valtype->kind);
    return false;
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
