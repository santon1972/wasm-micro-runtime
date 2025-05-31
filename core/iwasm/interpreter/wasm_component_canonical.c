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

// START: UTF-16 Transcoding Helpers
// Helper for UTF-16LE (wasm) to UTF-8 (host)
static char*
transcode_utf16le_to_utf8_on_host(const uint16 *utf16_ptr, uint32 utf16_code_units,
                                  uint32 *out_utf8_len_bytes,
                                  char *error_buf, uint32 error_buf_size)
{
    if (!utf16_ptr && utf16_code_units > 0) {
        set_canon_error(error_buf, error_buf_size, "transcode_utf16le_to_utf8_on_host: Null utf16_ptr for non-zero length.");
        if(out_utf8_len_bytes) *out_utf8_len_bytes = 0;
        return NULL;
    }
    if (!out_utf8_len_bytes) {
        set_canon_error(error_buf, error_buf_size, "transcode_utf16le_to_utf8_on_host: out_utf8_len_bytes is null.");
        return NULL;
    }
    *out_utf8_len_bytes = 0;

    if (utf16_code_units == 0) {
        char *empty_str = loader_malloc(1, error_buf, error_buf_size);
        if (!empty_str) return NULL; // Error set by loader_malloc
        empty_str[0] = '\0';
        return empty_str;
    }

    // Calculate exact UTF-8 length needed
    uint32 required_utf8_bytes = 0;
    for (uint32 i = 0; i < utf16_code_units; ++i) {
        uint16 c1 = utf16_ptr[i];
        if (c1 < 0x80) required_utf8_bytes += 1;
        else if (c1 < 0x800) required_utf8_bytes += 2;
        else if (c1 >= 0xD800 && c1 <= 0xDBFF) { // High surrogate
            if (i + 1 < utf16_code_units) {
                uint16 c2 = utf16_ptr[i + 1];
                if (c2 >= 0xDC00 && c2 <= 0xDFFF) { // Low surrogate
                    required_utf8_bytes += 4;
                    i++; // Consume low surrogate as well
                } else { required_utf8_bytes += 3; } // Treat as replacement char ?
            } else { required_utf8_bytes += 3; } // Unpaired high surrogate
        } else { required_utf8_bytes += 3; } // Includes unpaired low surrogates (treated as replacement char)
    }

    char *utf8_str = loader_malloc(required_utf8_bytes + 1, error_buf, error_buf_size);
    if (!utf8_str) return NULL;

    uint32 utf8_idx = 0;
    uint32 utf16_idx = 0;

    while (utf16_idx < utf16_code_units) {
        uint16 c1 = utf16_ptr[utf16_idx++];
        uint32 code_point;

        if (c1 >= 0xD800 && c1 <= 0xDBFF) { // High surrogate
            if (utf16_idx < utf16_code_units) {
                uint16 c2 = utf16_ptr[utf16_idx++];
                if (c2 >= 0xDC00 && c2 <= 0xDFFF) { // Low surrogate
                    code_point = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                } else { // Unpaired high surrogate
                    code_point = 0xFFFD; // Replacement character
                    utf16_idx--; // Re-process c2 if it wasn't a low surrogate
                }
            } else { // Unpaired high surrogate at end of string
                code_point = 0xFFFD; // Replacement character
            }
        } else if (c1 >= 0xDC00 && c1 <= 0xDFFF) { // Unpaired low surrogate
            code_point = 0xFFFD; // Replacement character
        } else { // BMP character
            code_point = c1;
        }

        // Encode code_point to UTF-8
        if (code_point <= 0x7F) {
            utf8_str[utf8_idx++] = (char)code_point;
        } else if (code_point <= 0x7FF) {
            utf8_str[utf8_idx++] = (char)(0xC0 | (code_point >> 6));
            utf8_str[utf8_idx++] = (char)(0x80 | (code_point & 0x3F));
        } else if (code_point <= 0xFFFF) {
            utf8_str[utf8_idx++] = (char)(0xE0 | (code_point >> 12));
            utf8_str[utf8_idx++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
            utf8_str[utf8_idx++] = (char)(0x80 | (code_point & 0x3F));
        } else if (code_point <= 0x10FFFF) {
            utf8_str[utf8_idx++] = (char)(0xF0 | (code_point >> 18));
            utf8_str[utf8_idx++] = (char)(0x80 | ((code_point >> 12) & 0x3F));
            utf8_str[utf8_idx++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
            utf8_str[utf8_idx++] = (char)(0x80 | (code_point & 0x3F));
        }
        // No need to check for overflow as required_utf8_bytes was pre-calculated
    }

    utf8_str[utf8_idx] = '\0';
    *out_utf8_len_bytes = utf8_idx;
    return utf8_str;
}

// Helper for UTF-8 (host) to UTF-16LE (temp host buffer for wasm)
static uint16*
transcode_utf8_to_utf16le_on_host(const char *utf8_str, uint32 utf8_len_bytes,
                                  uint32 *out_utf16_code_units,
                                  char *error_buf, uint32 error_buf_size)
{
    if (!utf8_str) {
        set_canon_error(error_buf, error_buf_size, "transcode_utf8_to_utf16le_on_host: Null utf8_str for transcoding.");
        if(out_utf16_code_units) *out_utf16_code_units = 0;
        return NULL;
    }
    if (!out_utf16_code_units) {
        set_canon_error(error_buf, error_buf_size, "transcode_utf8_to_utf16le_on_host: out_utf16_code_units is null.");
        return NULL;
    }
    *out_utf16_code_units = 0;

    if (utf8_len_bytes == 0) {
        // Return non-NULL for empty string, as Wasm side might still expect an allocation (e.g. offset, 0_len)
        // Allocate for 0 code units. loader_malloc(0) behavior can be platform-dependent.
        // It's safer to request 0 actual code units, and the caller handles Wasm allocation.
        return loader_malloc(0, error_buf, error_buf_size); // This might return NULL or a unique pointer.
    }

    // Calculate exact UTF-16 code units needed
    uint32 required_utf16_code_units = 0;
    uint32 temp_utf8_idx = 0;
    while(temp_utf8_idx < utf8_len_bytes) {
        uint8 c = utf8_str[temp_utf8_idx];
        if (c < 0x80) temp_utf8_idx += 1;
        else if ((c & 0xE0) == 0xC0) temp_utf8_idx += 2;
        else if ((c & 0xF0) == 0xE0) temp_utf8_idx += 3;
        else if ((c & 0xF8) == 0xF0) { temp_utf8_idx += 4; required_utf16_code_units++; /* For surrogate pair */ }
        else { set_canon_error(error_buf, error_buf_size, "Invalid UTF-8 sequence during length calculation."); return NULL; }
        if (temp_utf8_idx > utf8_len_bytes && c < 0xF0) { /* check overran only if not starting 4-byte seq */
            set_canon_error(error_buf, error_buf_size, "Invalid UTF-8 sequence (overrun) during length calculation."); return NULL;
        }
        required_utf16_code_units++;
    }


    uint16 *utf16_buf = loader_malloc(required_utf16_code_units * sizeof(uint16), error_buf, error_buf_size);
    if (!utf16_buf) return NULL;

    uint32 utf8_idx = 0;
    uint32 utf16_idx = 0;

    while (utf8_idx < utf8_len_bytes) {
        uint32 code_point;
        uint8 c1 = utf8_str[utf8_idx];
        uint32 consumed_utf8_bytes = 0;

        if (c1 < 0x80) {
            code_point = c1; consumed_utf8_bytes = 1;
        } else if ((c1 & 0xE0) == 0xC0) {
            if (utf8_idx + 1 >= utf8_len_bytes) goto invalid_utf8;
            uint8 c2 = utf8_str[utf8_idx + 1];
            if ((c2 & 0xC0) != 0x80) goto invalid_utf8;
            code_point = ((c1 & 0x1F) << 6) | (c2 & 0x3F); consumed_utf8_bytes = 2;
        } else if ((c1 & 0xF0) == 0xE0) {
            if (utf8_idx + 2 >= utf8_len_bytes) goto invalid_utf8;
            uint8 c2 = utf8_str[utf8_idx + 1]; uint8 c3 = utf8_str[utf8_idx + 2];
            if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) goto invalid_utf8;
            code_point = ((c1 & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F); consumed_utf8_bytes = 3;
        } else if ((c1 & 0xF8) == 0xF0) {
            if (utf8_idx + 3 >= utf8_len_bytes) goto invalid_utf8;
            uint8 c2 = utf8_str[utf8_idx + 1]; uint8 c3 = utf8_str[utf8_idx + 2]; uint8 c4 = utf8_str[utf8_idx + 3];
            if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80 || (c4 & 0xC0) != 0x80) goto invalid_utf8;
            code_point = ((c1 & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F); consumed_utf8_bytes = 4;
        } else { goto invalid_utf8; }

        utf8_idx += consumed_utf8_bytes;

        if (code_point <= 0xFFFF) {
            utf16_buf[utf16_idx++] = (uint16)code_point;
        } else if (code_point <= 0x10FFFF) {
            code_point -= 0x10000;
            utf16_buf[utf16_idx++] = (uint16)((code_point >> 10) + 0xD800);
            utf16_buf[utf16_idx++] = (uint16)((code_point & 0x3FF) + 0xDC00);
        } else { goto invalid_utf8; } // Should not happen if UTF-8 is valid and up to 4 bytes
    }

    *out_utf16_code_units = utf16_idx;
    return utf16_buf;

invalid_utf8:
    loader_free(utf16_buf);
    set_canon_error(error_buf, error_buf_size, "Invalid UTF-8 sequence during transcoding to UTF-16LE.");
    return NULL;
}
// END: UTF-16 Transcoding Helpers

// Helper to convert primitive value type enum to string for error messages
static const char* primitive_val_type_to_string(WASMComponentPrimValType ptype) {
    switch (ptype) {
        case PRIM_VAL_BOOL: return "bool";
        case PRIM_VAL_S8: return "s8";
        case PRIM_VAL_U8: return "u8";
        case PRIM_VAL_S16: return "s16";
        case PRIM_VAL_U16: return "u16";
        case PRIM_VAL_S32: return "s32";
        case PRIM_VAL_U32: return "u32";
        case PRIM_VAL_S64: return "s64";
        case PRIM_VAL_U64: return "u64";
        case PRIM_VAL_F32: return "f32";
        case PRIM_VAL_F64: return "f64";
        case PRIM_VAL_CHAR: return "char";
        case PRIM_VAL_STRING: return "string";
        default: return "unknown_primitive";
    }
}


/* Resource Handling Globals */
#define MAX_RESOURCE_HANDLES 128 // Example size

typedef struct WAMRHostResourceEntry {
    bool is_active;
    uint32 component_resource_type_idx; // Index into component->type_definitions where kind is DEF_TYPE_KIND_RESOURCE
    void *host_data;                    // For host-managed opaque data associated with the handle

    // --- New fields for Destructor Support ---
    WASMModuleInstance *owner_module_inst;  // Module instance that owns the destructor
    uint32 dtor_core_func_idx;          // Core function index of the destructor within owner_module_inst
                                        // Set to (uint32)-1 if no destructor
} WAMRHostResourceEntry;

static WAMRHostResourceEntry global_resource_table[MAX_RESOURCE_HANDLES];
static uint32_t next_resource_handle = 1; // Start handles from 1 for easier debugging (0 can be invalid)
static bool resource_table_initialized = false;

static void initialize_resource_table() {
    if (!resource_table_initialized) {
        memset(global_resource_table, 0, sizeof(global_resource_table));
        // global_resource_table[0] is implicitly inactive due to memset and handle logic starting from 1.
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
                uint32 length = core_params[1]; // For UTF-8, this is bytes. For UTF-16, this is number of code units.

                uint8 *core_mem_base = wasm_runtime_get_memory_ptr(module_inst, mem_idx, NULL);
                if (!core_mem_base) {
                     set_canon_error(error_buf, error_buf_size, "Failed to get memory pointer for string lifting.");
                     return false;
                }
                
                WASMComponentCanonicalOptionKind string_encoding = CANONICAL_OPTION_STRING_ENCODING_UTF8; // Default
                if (canonical_def) {
                    for (uint32 opt_idx = 0; opt_idx < canonical_def->option_count; ++opt_idx) {
                        if (canonical_def->options[opt_idx].kind == CANONICAL_OPTION_STRING_ENCODING_UTF8 ||
                            canonical_def->options[opt_idx].kind == CANONICAL_OPTION_STRING_ENCODING_UTF16 ||
                            canonical_def->options[opt_idx].kind == CANONICAL_OPTION_STRING_ENCODING_LATIN1_UTF16) {
                            string_encoding = canonical_def->options[opt_idx].kind;
                            break;
                        }
                    }
                }

                char *lifted_str_ptr = NULL;
                uint32 lifted_str_len_bytes = 0; // Length in bytes of the final UTF-8 string on host

                if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF8) {
                    if (!wasm_runtime_validate_app_addr(module_inst, mem_idx, offset, length)) {
                        set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for UTF-8 string at offset %u, length %u", offset, length);
                        return false;
                    }
                    lifted_str_len_bytes = length;
                    lifted_str_ptr = loader_malloc(lifted_str_len_bytes + 1, error_buf, error_buf_size);
                    if (!lifted_str_ptr) return false;
                    bh_memcpy_s(lifted_str_ptr, lifted_str_len_bytes + 1, core_mem_base + offset, length);
                    lifted_str_ptr[lifted_str_len_bytes] = '\0';
                } else if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16) {
                    // length is number of UTF-16 code units. Total bytes in wasm = length * 2.
                    uint32 utf16_bytes_in_wasm = length * sizeof(uint16);
                    if (!wasm_runtime_validate_app_addr(module_inst, mem_idx, offset, utf16_bytes_in_wasm)) {
                        set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for UTF-16 string at offset %u, code_units %u (%u bytes)", offset, length, utf16_bytes_in_wasm);
                        return false;
                    }
                    uint16 *utf16_wasm_ptr = (uint16*)(core_mem_base + offset);
                    lifted_str_ptr = transcode_utf16le_to_utf8_on_host(utf16_wasm_ptr, length, /* length is utf16_code_units */
                                                                     &lifted_str_len_bytes,
                                                                     error_buf, error_buf_size);
                    if (!lifted_str_ptr) {
                        // error_buf is set by transcoder
                        return false;
                    }
                } else if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_UTF16) {
                    // TODO: Implement inspection and transcoding for Latin1+UTF16
                    LOG_WARNING("String lifting for Latin1+UTF16: Currently treating as UTF-8 if it were Latin1, or potentially misinterpreting if it's UTF-16. Full support pending.");
                    // Fallback to UTF-8 like behavior for now, or specific error.
                    // For now, let's be explicit this is not fully supported.
                    // Duplicating UTF-8 logic here as a placeholder for "treat as bytes"
                    if (!wasm_runtime_validate_app_addr(module_inst, mem_idx, offset, length)) {
                        set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for Latin1+UTF16 (treated as bytes) string at offset %u, length %u", offset, length);
                        return false;
                    }
                    lifted_str_len_bytes = length; // Assuming length is bytes for this path
                    lifted_str_ptr = loader_malloc(lifted_str_len_bytes + 1, error_buf, error_buf_size);
                    if (!lifted_str_ptr) return false;
                    bh_memcpy_s(lifted_str_ptr, lifted_str_len_bytes + 1, core_mem_base + offset, length);
                    lifted_str_ptr[lifted_str_len_bytes] = '\0';
                    // set_canon_error(error_buf, error_buf_size, "Latin1+UTF16 string lifting not yet implemented.");
                    // return false;
                } else {
                     set_canon_error_v(error_buf, error_buf_size, "Unknown string encoding option: %d", string_encoding);
                     return false;
                }
                
                char **result_ptr_loc = loader_malloc(sizeof(char*), error_buf, error_buf_size);
                if (!result_ptr_loc) { 
                    if (lifted_str_ptr) loader_free(lifted_str_ptr); 
                    return false; 
                }
                *result_ptr_loc = lifted_str_ptr;
                *lifted_component_value_ptr = result_ptr_loc;
                
                return true;
            }
            default:
                set_canon_error_v(error_buf, error_buf_size, "Unsupported primitive type '%s' (tag %d) for lifting", 
                                  primitive_val_type_to_string(target_component_valtype->u.primitive), 
                                  target_component_valtype->u.primitive);
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
        uint8 *core_mem_base = wasm_runtime_get_memory_ptr(module_inst, mem_idx, NULL);
        if (!core_mem_base) {
             set_canon_error(error_buf, error_buf_size, "Failed to get memory pointer for list lifting.");
             return false;
        }

        HostComponentList *host_list_struct = loader_malloc(sizeof(HostComponentList), error_buf, error_buf_size);
        if (!host_list_struct) return false;
        host_list_struct->count = list_length;
        host_list_struct->elements = NULL;

        if (list_length > 0) {
            host_list_struct->elements = loader_malloc(list_length * sizeof(void*), error_buf, error_buf_size);
            if (!host_list_struct->elements) {
                loader_free(host_list_struct);
                return false;
            }
            memset(host_list_struct->elements, 0, list_length * sizeof(void*));

            uint32 current_element_data_offset_within_list_buffer = 0;
            for (uint32 i = 0; i < list_length; ++i) {
                uint32 element_core_size, element_core_align;
                if (!get_component_type_core_abi_details(element_valtype, module_inst, &element_core_size, &element_core_align, error_buf, error_buf_size)) {
                    for(uint32 j=0; j<i; ++j) if(host_list_struct->elements[j]) loader_free(host_list_struct->elements[j]); // Assuming elements are simple mallocs
                    loader_free(host_list_struct->elements);
                    loader_free(host_list_struct);
                    return false;
                }

                current_element_data_offset_within_list_buffer = align_up(current_element_data_offset_within_list_buffer, element_core_align);

                void *core_elem_addr = core_mem_base + list_offset + current_element_data_offset_within_list_buffer;

                if (!wasm_runtime_validate_app_addr(module_inst, mem_idx, list_offset + current_element_data_offset_within_list_buffer, element_core_size)) {
                     set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for list element %u at offset %u, size %u", i, list_offset + current_element_data_offset_within_list_buffer, element_core_size);
                     for(uint32 j=0; j<i; ++j) if(host_list_struct->elements[j]) loader_free(host_list_struct->elements[j]);
                     loader_free(host_list_struct->elements);
                     loader_free(host_list_struct);
                     return false;
                }

                uint8 core_element_type_tag_for_recursive_lift = VALUE_TYPE_I32; // Default for complex types (offset)
                if (element_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                    core_element_type_tag_for_recursive_lift = get_core_wasm_type_for_primitive(element_valtype->u.primitive);
                }


                if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                    core_elem_addr, core_element_type_tag_for_recursive_lift,
                                                    element_valtype,
                                                    &host_list_struct->elements[i],
                                                    error_buf, error_buf_size)) {
                    for (uint32 j = 0; j <= i; ++j) { // Free up to and including the one that failed if it allocated
                        if (host_list_struct->elements[j]) loader_free(host_list_struct->elements[j]);
                    }
                    loader_free(host_list_struct->elements);
                    loader_free(host_list_struct);
                    return false;
                }
                current_element_data_offset_within_list_buffer += element_core_size;
            }
        }
        *lifted_component_value_ptr = host_list_struct;
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
            
            // Determine core type of payload for recursive call
            uint8 payload_core_type_tag_for_lift = VALUE_TYPE_I32; // Default for complex types (offset)
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                 payload_core_type_tag_for_lift = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            } else if (payload_valtype->kind == VAL_TYPE_KIND_STRING || payload_valtype->kind == VAL_TYPE_KIND_LIST) {
                // String/List are (ptr, len) pairs, effectively i32 for the recursive call to get the pair itself
                 payload_core_type_tag_for_lift = VALUE_TYPE_I32;
            }


            if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                payload_wasm_addr, payload_core_type_tag_for_lift,
                                                payload_valtype, 
                                                &host_option->val, // Store lifted payload ptr here
                                                error_buf, error_buf_size)) {
                loader_free(host_option);
                return false; // Error message is already set by the recursive call.
            }
        } else if (disc != 0 && disc != 1) { // Option discriminant must be 0 or 1
            loader_free(host_option);
            set_canon_error_v(error_buf, error_buf_size, "Invalid discriminant %u for option type", disc);
            return false;
        }
        // If disc is 0 (None), host_option->val remains NULL, which is correct.
        
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

            uint8 payload_core_type_tag_for_lift = VALUE_TYPE_I32; // Default for complex types
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                 payload_core_type_tag_for_lift = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            } else if (payload_valtype->kind == VAL_TYPE_KIND_STRING || payload_valtype->kind == VAL_TYPE_KIND_LIST) {
                 payload_core_type_tag_for_lift = VALUE_TYPE_I32;
            }

            if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                payload_wasm_addr, payload_core_type_tag_for_lift,
                                                payload_valtype, 
                                                &host_result->val,
                                                error_buf, error_buf_size)) {
                loader_free(host_result);
                return false; // Error message is already set by the recursive call.
            }
        }
        // If payload_valtype is NULL (e.g. for result<void, E> or result<T, void>), host_result->val remains NULL.
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
            
            uint8 payload_core_type_tag_for_lift = VALUE_TYPE_I32; // Default for complex types
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                 payload_core_type_tag_for_lift = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            } else if (payload_valtype->kind == VAL_TYPE_KIND_STRING || payload_valtype->kind == VAL_TYPE_KIND_LIST) {
                 payload_core_type_tag_for_lift = VALUE_TYPE_I32;
            }

            if (!wasm_component_canon_lift_value(exec_env, canonical_def, core_func_idx,
                                                payload_wasm_addr, payload_core_type_tag_for_lift,
                                                payload_valtype, 
                                                &host_variant->val,
                                                error_buf, error_buf_size)) {
                loader_free(host_variant);
                return false; // Error message is already set by the recursive call.
            }
        }
        // If !payload_valtype (case has no payload), host_variant->val remains NULL.
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

            // core_value_ptr points to an i32 Wasm offset of the tuple data.
            if (core_value_type != VALUE_TYPE_I32) {
                set_canon_error_v(error_buf, error_buf_size, "Tuple lifting expects core_value_type I32 (for offset), got %u", core_value_type);
                if (lifted_elements_array && tuple_type->field_count > 0) loader_free(lifted_elements_array);
                return false;
            }
            if (mem_idx == (uint32)-1 && canonical_def) {
                 set_canon_error(error_buf, error_buf_size, "Tuple lifting from memory requires memory option.");
                 if (lifted_elements_array && tuple_type->field_count > 0) loader_free(lifted_elements_array);
                 return false;
            }
            if (!module_inst) {
                 set_canon_error(error_buf, error_buf_size, "Module instance required for tuple lifting from memory.");
                 if (lifted_elements_array && tuple_type->field_count > 0) loader_free(lifted_elements_array);
                 return false;
            }
            uint8 *core_mem_base = wasm_runtime_get_memory_ptr(module_inst, mem_idx, NULL);
            if (!core_mem_base) {
                 set_canon_error(error_buf, error_buf_size, "Failed to get memory pointer for tuple lifting.");
                 if (lifted_elements_array && tuple_type->field_count > 0) loader_free(lifted_elements_array);
                 return false;
            }

            uint32 tuple_offset_in_wasm = *(uint32*)core_value_ptr;
            uint32 current_offset_within_tuple_buffer = 0;

            for (uint32 i = 0; i < tuple_type->field_count; ++i) {
                // The type of each element is directly in element_valtypes array
                WASMComponentValType *element_val_type = &tuple_type->element_valtypes[i];
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
    } else if (target_component_valtype->kind == VAL_TYPE_KIND_FLAGS) {
        // core_value_ptr points to the start of the i32(s) in Wasm memory.
        // Host representation: uint32_t* (array of uint32_t, each holding up to 32 flags)
        uint32 label_count = target_component_valtype->u.flags.label_count;
        if (label_count == 0) {
            *lifted_component_value_ptr = NULL; // No data to lift for zero flags
            return true;
        }

        uint32 num_i32s = (label_count + 31) / 32;
        uint32 total_size_bytes = num_i32s * sizeof(uint32);

        // LOG_TODO: Direct memory validation for flags lifting from core_value_ptr is complex
        // as core_value_ptr might be a native pointer to Wasm memory or an offset,
        // and its exact nature/origin needs careful handling for wasm_runtime_validate_app_addr.
        // Currently skipped, proceeding with the assumption that core_value_ptr is valid.
        // Example of commented out validation:
        // if (module_inst && mem_idx != (uint32)-1) {
        //     // This requires core_value_ptr to be an app_offset, not a native_ptr.
        //     if (!wasm_runtime_validate_app_addr(module_inst, mem_idx, (uint32)(uintptr_t)core_value_ptr, total_size_bytes)) {
        //          set_canon_error_v(error_buf, error_buf_size, "Invalid memory access for flags lifting at offset %p, size %u", core_value_ptr, total_size_bytes);
        //          return false;
        //     }
        // }


        uint32_t *host_flags_array = loader_malloc(total_size_bytes, error_buf, error_buf_size);
        if (!host_flags_array) {
            return false; // Error set by loader_malloc
        }

        bh_memcpy_s(host_flags_array, total_size_bytes, core_value_ptr, total_size_bytes);
        
        // The lifted value is the array of uint32_t itself.
        // If the host ABI expects a pointer to this array (e.g. uint32_t**), then another allocation is needed.
        // Current convention for primitives (like i32) is to return a pointer to the value (e.g. int*).
        // For consistency with how strings (char**) or lists (void**) are handled (pointer to the data structure),
        // flags could be seen as a fixed-size array of u32s, so uint32_t* seems appropriate.
        *lifted_component_value_ptr = host_flags_array;
        return true;
    }  else if (source_component_valtype->kind == VAL_TYPE_KIND_FLAGS) {
        // component_value_ptr points to the host-side uint32_t* array.
        // core_value_write_ptr is where the i32(s) should be written in Wasm memory.
        // target_core_wasm_type is not directly used here as flags map to a sequence of i32s.

        uint32 label_count = source_component_valtype->u.flags.label_count;
        if (label_count == 0) {
            // No data to write for zero flags. core_value_write_ptr might not even be valid if size is 0.
            return true;
        }

        uint32 num_i32s = (label_count + 31) / 32;
        uint32 total_size_bytes = num_i32s * sizeof(uint32);

        if (!component_value_ptr) {
            set_canon_error(error_buf, error_buf_size, "Flags lowering received null component_value_ptr for non-empty flags.");
            return false;
        }
        if (!core_value_write_ptr) {
             set_canon_error(error_buf, error_buf_size, "Flags lowering received null core_value_write_ptr for non-empty flags.");
            return false;
        }
        
        // LOG_TODO: Direct memory validation for flags lowering to core_value_write_ptr is complex
        // as it requires knowing the Wasm app_offset that core_value_write_ptr corresponds to
        // for wasm_runtime_validate_app_addr. Currently skipped.

        bh_memcpy_s(core_value_write_ptr, total_size_bytes, component_value_ptr, total_size_bytes);
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
                char *str_to_lower = *(char**)component_value_ptr; 
                if (!str_to_lower) {
                    set_canon_error(error_buf, error_buf_size, "Cannot lower null string pointer.");
                    return false;
                }
                
                WASMComponentCanonicalOptionKind string_encoding = CANONICAL_OPTION_STRING_ENCODING_UTF8; // Default
                if (canonical_def) {
                    for (uint32 opt_idx = 0; opt_idx < canonical_def->option_count; ++opt_idx) {
                        if (canonical_def->options[opt_idx].kind == CANONICAL_OPTION_STRING_ENCODING_UTF8 ||
                            canonical_def->options[opt_idx].kind == CANONICAL_OPTION_STRING_ENCODING_UTF16 ||
                            canonical_def->options[opt_idx].kind == CANONICAL_OPTION_STRING_ENCODING_LATIN1_UTF16) {
                            string_encoding = canonical_def->options[opt_idx].kind;
                            break;
                        }
                    }
                }

                uint32 wasm_offset = 0;
                uint32 wasm_len = 0; // For UTF-8: bytes; For UTF-16: code units
                void *alloc_native_ptr = NULL;

                if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF8) {
                    wasm_len = strlen(str_to_lower);
                    uint32 alloc_size_bytes = wasm_len;

                    if (use_wasm_realloc) {
                        uint32 argv[4] = {0, 0, 1, alloc_size_bytes}; // old_ptr, old_size, align, new_size
                        WASMFunctionInstance *realloc_f = wasm_runtime_get_function(module_inst, realloc_func_idx);
                        if (!realloc_f || !wasm_runtime_call_wasm(exec_env, realloc_f, 4, argv)) {
                            set_canon_error_v(error_buf, error_buf_size, "Wasm realloc failed for UTF-8 string. Error: %s", wasm_runtime_get_exception(module_inst)); return false;
                        }
                        wasm_offset = argv[0];
                        if (wasm_offset == 0 && alloc_size_bytes > 0) { set_canon_error(error_buf, error_buf_size, "Wasm realloc returned 0 for UTF-8 string."); return false; }
                        alloc_native_ptr = wasm_runtime_addr_app_to_native(module_inst, mem_idx, wasm_offset);
                    } else {
                        alloc_native_ptr = wasm_runtime_module_malloc(module_inst, alloc_size_bytes, (void**)&wasm_offset);
                    }
                    if (!alloc_native_ptr && alloc_size_bytes > 0) { set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes for UTF-8 string.", alloc_size_bytes); return false; }
                    if (alloc_native_ptr) bh_memcpy_s(alloc_native_ptr, alloc_size_bytes, str_to_lower, wasm_len);

                } else if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_UTF16) {
                    uint32 utf8_byte_len = strlen(str_to_lower);
                    uint16 *temp_utf16_host_buffer = transcode_utf8_to_utf16le_on_host(str_to_lower, utf8_byte_len,
                                                                                     &wasm_len, /* out: utf16_code_units */
                                                                                     error_buf, error_buf_size);
                    if (!temp_utf16_host_buffer && utf8_byte_len > 0) { // Transcoding failed
                        // error_buf set by transcoder
                        return false;
                    }

                    uint32 alloc_size_bytes = wasm_len * sizeof(uint16);

                    if (use_wasm_realloc) {
                        uint32 argv[4] = {0, 0, sizeof(uint16), alloc_size_bytes}; // old_ptr, old_size, align(u16), new_size
                        WASMFunctionInstance *realloc_f = wasm_runtime_get_function(module_inst, realloc_func_idx);
                        if (!realloc_f || !wasm_runtime_call_wasm(exec_env, realloc_f, 4, argv)) {
                            if(temp_utf16_host_buffer) loader_free(temp_utf16_host_buffer);
                            set_canon_error_v(error_buf, error_buf_size, "Wasm realloc failed for UTF-16 string. Error: %s", wasm_runtime_get_exception(module_inst)); return false;
                        }
                        wasm_offset = argv[0];
                        if (wasm_offset == 0 && alloc_size_bytes > 0) {
                            if(temp_utf16_host_buffer) loader_free(temp_utf16_host_buffer);
                            set_canon_error(error_buf, error_buf_size, "Wasm realloc returned 0 for UTF-16 string."); return false;
                        }
                        alloc_native_ptr = wasm_runtime_addr_app_to_native(module_inst, mem_idx, wasm_offset);
                    } else {
                        alloc_native_ptr = wasm_runtime_module_malloc(module_inst, alloc_size_bytes, (void**)&wasm_offset);
                    }

                    if (!alloc_native_ptr && alloc_size_bytes > 0) {
                        if(temp_utf16_host_buffer) loader_free(temp_utf16_host_buffer);
                        set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes for UTF-16 string in Wasm.", alloc_size_bytes); return false;
                    }
                    if (alloc_native_ptr && temp_utf16_host_buffer && alloc_size_bytes > 0) {
                         bh_memcpy_s(alloc_native_ptr, alloc_size_bytes, temp_utf16_host_buffer, alloc_size_bytes);
                    }
                    if(temp_utf16_host_buffer) loader_free(temp_utf16_host_buffer);

                } else if (string_encoding == CANONICAL_OPTION_STRING_ENCODING_LATIN1_UTF16) {
                    LOG_WARNING("String lowering for Latin1+UTF16: Currently treating as UTF-8. Full support pending.");
                    // Fallback to UTF-8 like behavior
                    wasm_len = strlen(str_to_lower); // byte length
                    uint32 alloc_size_bytes = wasm_len;
                    if (use_wasm_realloc) {
                        uint32 argv[4] = {0, 0, 1, alloc_size_bytes};
                        WASMFunctionInstance *realloc_f = wasm_runtime_get_function(module_inst, realloc_func_idx);
                        if (!realloc_f || !wasm_runtime_call_wasm(exec_env, realloc_f, 4, argv)) { set_canon_error_v(error_buf, error_buf_size, "Wasm realloc failed for Latin1+UTF16 (as UTF-8). Error: %s", wasm_runtime_get_exception(module_inst)); return false; }
                        wasm_offset = argv[0];
                        if (wasm_offset == 0 && alloc_size_bytes > 0) { set_canon_error(error_buf, error_buf_size, "Wasm realloc returned 0 for Latin1+UTF16 (as UTF-8)."); return false; }
                        alloc_native_ptr = wasm_runtime_addr_app_to_native(module_inst, mem_idx, wasm_offset);
                    } else {
                        alloc_native_ptr = wasm_runtime_module_malloc(module_inst, alloc_size_bytes, (void**)&wasm_offset);
                    }
                    if (!alloc_native_ptr && alloc_size_bytes > 0) { set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes for Latin1+UTF16 (as UTF-8).", alloc_size_bytes); return false; }
                    if (alloc_native_ptr) bh_memcpy_s(alloc_native_ptr, alloc_size_bytes, str_to_lower, wasm_len);
                    // Note: wasm_len here is byte length. If the intent for Latin1+UTF16 is to pass UTF-16 code units, this is incorrect.
                    // For now, it mirrors UTF-8, meaning 'length' is byte count.
                    // set_canon_error(error_buf, error_buf_size, "Latin1+UTF16 string lowering not yet implemented.");
                    // return false;
                } else {
                    set_canon_error_v(error_buf, error_buf_size, "Unknown string encoding option for lowering: %d", string_encoding);
                    return false;
                }
                
                uint32_t *out_pair = (uint32_t*)core_value_write_ptr;
                out_pair[0] = wasm_offset;
                out_pair[1] = wasm_len; 
                
                LOG_VERBOSE("Lowered string (encoding %d) to wasm mem offset %u, length %u (units)", string_encoding, wasm_offset, wasm_len);
                return true;
            }
            default:
                set_canon_error_v(error_buf, error_buf_size, "Unsupported primitive type '%s' (tag %d) for lowering", 
                                  primitive_val_type_to_string(source_component_valtype->u.primitive),
                                  source_component_valtype->u.primitive);
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
        uint32 total_wasm_elements_alloc_size = 0;
        uint32 list_max_align = 1;

        // Calculate total size and max alignment for the list elements buffer in Wasm
        if (host_list->count > 0) {
            for (uint32 i = 0; i < host_list->count; ++i) {
                uint32 element_core_size, element_core_align;
                if (!get_component_type_core_abi_details(element_valtype, module_inst, &element_core_size, &element_core_align, error_buf, error_buf_size)) {
                    return false; // Error set by helper
                }
                if (element_core_align > list_max_align) list_max_align = element_core_align;
                total_wasm_elements_alloc_size = align_up(total_wasm_elements_alloc_size, element_core_align);
                total_wasm_elements_alloc_size += element_core_size;
            }
            if (list_max_align == 0) list_max_align = 1; // Should not happen if elements have types
            total_wasm_elements_alloc_size = align_up(total_wasm_elements_alloc_size, list_max_align);
        }

        uint32 wasm_list_elements_offset = 0;
        void *wasm_list_elements_native_ptr = NULL;

        if (total_wasm_elements_alloc_size > 0) {
            if (use_wasm_realloc) {
                uint32 argv[4] = {0, 0, list_max_align, total_wasm_elements_alloc_size};
                WASMFunctionInstance *realloc_f = wasm_runtime_get_function(module_inst, realloc_func_idx);
                if (!realloc_f || !wasm_runtime_call_wasm(exec_env, realloc_f, 4, argv)) {
                    set_canon_error_v(error_buf, error_buf_size, "Wasm realloc failed for list elements. Error: %s", wasm_runtime_get_exception(module_inst)); return false;
                }
                wasm_list_elements_offset = argv[0];
                if (wasm_list_elements_offset == 0 && total_wasm_elements_alloc_size > 0) { set_canon_error(error_buf, error_buf_size, "Wasm realloc returned 0 for list elements."); return false; }
                wasm_list_elements_native_ptr = wasm_runtime_addr_app_to_native(module_inst, mem_idx, wasm_list_elements_offset);
            } else {
                wasm_list_elements_native_ptr = wasm_runtime_module_malloc(module_inst, total_wasm_elements_alloc_size, (void**)&wasm_list_elements_offset);
            }
            if (!wasm_list_elements_native_ptr && total_wasm_elements_alloc_size > 0) {
                set_canon_error_v(error_buf, error_buf_size, "Failed to allocate %u bytes for list elements in Wasm.", total_wasm_elements_alloc_size); return false;
            }
        }
        
        uint32 current_element_write_offset_in_buffer = 0;
        for (uint32 i = 0; i < host_list->count; ++i) {
            void *host_elem_ptr = host_list->elements[i];
            uint32 element_core_size, element_core_align; // Recalculate for current element to advance offset correctly
            if (!get_component_type_core_abi_details(element_valtype, module_inst, &element_core_size, &element_core_align, error_buf, error_buf_size)) {
                // This would be an internal error if previous loop succeeded.
                // Free Wasm buffer if allocated.
                if (wasm_list_elements_native_ptr && total_wasm_elements_alloc_size > 0) {
                    if (use_wasm_realloc) { /* Leak warning as before */ } else { wasm_runtime_module_free(module_inst, wasm_list_elements_offset); }
                }
                return false;
            }

            current_element_write_offset_in_buffer = align_up(current_element_write_offset_in_buffer, element_core_align);
            void *core_elem_write_ptr_in_wasm = (uint8*)wasm_list_elements_native_ptr + current_element_write_offset_in_buffer;

            uint8 element_target_core_type = VALUE_TYPE_I32; // Default for complex types
            if (element_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                element_target_core_type = get_core_wasm_type_for_primitive(element_valtype->u.primitive);
            }

            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_elem_ptr, element_valtype,
                                                  element_target_core_type,
                                                  core_elem_write_ptr_in_wasm, 
                                                  error_buf, error_buf_size)) {
                if (wasm_list_elements_native_ptr && total_wasm_elements_alloc_size > 0) {
                     if (use_wasm_realloc) { /* Leak warning */ } else { wasm_runtime_module_free(module_inst, wasm_list_elements_offset); }
                }
                return false;
            }
            current_element_write_offset_in_buffer += element_core_size;
        }

        uint32_t *out_pair = (uint32_t*)core_value_write_ptr;
        out_pair[0] = wasm_list_elements_offset;
        out_pair[1] = host_list->count; 
        
        LOG_VERBOSE("Lowered list to wasm mem offset %u, element_count %u, total element data size %u", wasm_list_elements_offset, host_list->count, total_wasm_elements_alloc_size);
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
        if (target_core_wasm_type == VALUE_TYPE_I32) { // Target for the record itself is a pointer (offset)
            *(uint32*)core_value_write_ptr = wasm_record_offset;
        } else {
            set_canon_error_v(error_buf, error_buf_size, "Record lowering expects target core type I32 for record offset, got %u", target_core_wasm_type);
            if (wasm_record_native_ptr && total_wasm_alloc_size > 0) wasm_runtime_module_free(module_inst, wasm_record_offset); // Free if allocated and not used
            loader_free(field_core_sizes); // Still need to free these helper arrays
            loader_free(field_core_alignments);
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

            uint8 payload_target_core_type = VALUE_TYPE_I32; // Default for complex types (offset/pair)
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                payload_target_core_type = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }
            // For VAL_TYPE_KIND_STRING or VAL_TYPE_KIND_LIST, target is still I32 (for the pair)
            
            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_option->val, payload_valtype,
                                                  payload_target_core_type,
                                                  payload_write_addr,
                                                  error_buf, error_buf_size)) {
                if (wasm_option_native_ptr && wasm_option_total_size > 0) wasm_runtime_module_free(module_inst, wasm_option_offset);
                return false; // Error message is set by recursive call.
            }
        } else if (host_option->disc != 0 && host_option->disc != 1) { // Invalid discriminant for option
             set_canon_error_v(error_buf, error_buf_size, "Invalid discriminant %u for host option value", host_option->disc);
             if (wasm_option_native_ptr && wasm_option_total_size > 0) wasm_runtime_module_free(module_inst, wasm_option_offset);
             return false;
        }
        // if disc is 0 (None), payload area in Wasm is allocated but not written to, which is fine.

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

            uint8 payload_target_core_type = VALUE_TYPE_I32; // Default for complex types
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                payload_target_core_type = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }
            // For VAL_TYPE_KIND_STRING or VAL_TYPE_KIND_LIST, target is still I32
            
            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_result->val, payload_valtype,
                                                  payload_target_core_type,
                                                  payload_write_addr,
                                                  error_buf, error_buf_size)) {
                if (wasm_result_native_ptr && wasm_result_total_size > 0) wasm_runtime_module_free(module_inst, wasm_result_offset);
                return false; // Error message propagated from recursive call
            }
        } else if (payload_valtype && !host_result->val) {
            LOG_VERBOSE("Host result has payload type but null value pointer for disc %u. Wasm payload area will be uninitialized.", host_result->disc);
        }
        // If !payload_valtype (e.g. result<void, E>), no payload processing needed.
        // If disc is invalid, it's caught earlier.

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

            uint8 payload_target_core_type = VALUE_TYPE_I32; // Default for complex types
            if (payload_valtype->kind == VAL_TYPE_KIND_PRIMITIVE) {
                payload_target_core_type = get_core_wasm_type_for_primitive(payload_valtype->u.primitive);
            }
             // For VAL_TYPE_KIND_STRING or VAL_TYPE_KIND_LIST, target is still I32

            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_variant->val, payload_valtype,
                                                  payload_target_core_type,
                                                  payload_write_addr,
                                                  error_buf, error_buf_size)) {
                if (wasm_variant_native_ptr && wasm_variant_total_size > 0) wasm_runtime_module_free(module_inst, wasm_variant_offset);
                return false; // Error propagated from recursive call
            }
        } else if (payload_valtype && !host_variant->val) { // Case has a type, but no data provided by host
             LOG_VERBOSE("Host variant (disc %u) has payload type but null value pointer. Wasm payload area will be uninitialized.", host_variant->disc);
        }
        // If !payload_valtype (case has no payload type), no payload processing needed.

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

            // Determine the target core type for the recursive call
            if (element_val_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
                element_target_core_type = get_core_wasm_type_for_primitive(element_val_type->u.primitive);
            } else { // Complex types like nested list, string, record, tuple, etc., are stored as i32 offset or (i32,i32) pair
                element_target_core_type = VALUE_TYPE_I32;
            }

            current_offset_in_wasm_struct = align_up(current_offset_in_wasm_struct, element_core_alignments[i]);
            void *core_elem_write_ptr_in_wasm = (uint8*)wasm_tuple_native_ptr + current_offset_in_wasm_struct;

            if (!wasm_component_canon_lower_value(exec_env, canonical_def, core_func_idx,
                                                  host_elem_ptr, element_val_type,
                                                  element_target_core_type,
                                                  core_elem_write_ptr_in_wasm, 
                                                  error_buf, error_buf_size)) {
                if (wasm_tuple_native_ptr && total_wasm_alloc_size > 0) wasm_runtime_module_free(module_inst, wasm_tuple_offset);
                if(element_core_sizes) loader_free(element_core_sizes);
                if(element_core_alignments) loader_free(element_core_alignments);
                return false;
            }
            current_offset_in_wasm_struct += element_core_sizes[i];
        }
        
        if(element_core_sizes) loader_free(element_core_sizes);
        if(element_core_alignments) loader_free(element_core_alignments);

        // 4. Write the Wasm offset of the tuple structure to core_value_write_ptr
        if (target_core_wasm_type == VALUE_TYPE_I32) { // Target for the tuple itself is a pointer (offset)
            *(uint32*)core_value_write_ptr = wasm_tuple_offset;
        } else {
            set_canon_error_v(error_buf, error_buf_size, "Tuple lowering expects target core type I32 for tuple offset, got %u", target_core_wasm_type);
            if (wasm_tuple_native_ptr && total_wasm_alloc_size > 0) wasm_runtime_module_free(module_inst, wasm_tuple_offset);
            // No need to free element_core_sizes/alignments again if already freed above on error
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
                set_canon_error_v(error_buf, error_buf_size, "Unsupported primitive type '%s' (tag %d) or zero size for ABI details", 
                                  primitive_val_type_to_string(val_type->u.primitive), val_type->u.primitive);
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
                if (field_alignment == 0) {
                    set_canon_error(error_buf, error_buf_size, "Tuple field alignment is zero.");
                    return false;
                }
                current_offset = align_up(current_offset, field_alignment);
                current_offset += field_size;
                if (field_alignment > max_align) {
                    max_align = field_alignment;
                }
            }
            if (max_align == 0) {
                set_canon_error(error_buf, error_buf_size, "Tuple max alignment is zero.");
                return false;
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
                if (field_alignment == 0) {
                    set_canon_error(error_buf, error_buf_size, "Record field alignment is zero.");
                    return false;
                }
                current_offset = align_up(current_offset, field_alignment);
                current_offset += field_size;
                if (field_alignment > max_align) {
                    max_align = field_alignment;
                }
            }
            if (max_align == 0) {
                set_canon_error(error_buf, error_buf_size, "Record max alignment is zero.");
                return false;
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
            if (val_align == 0) { // val_align comes from the payload type. If it's 0, it's an issue.
                set_canon_error(error_buf, error_buf_size, "Option payload alignment (val_align) is zero.");
                return false;
            }
            uint32 payload_offset = align_up(disc_size, val_align);
            *out_size = payload_offset + val_size;
            *out_alignment = disc_align > val_align ? disc_align : val_align; // Max of discriminant and value alignment
            if (*out_alignment == 0) {
                set_canon_error(error_buf, error_buf_size, "Option calculated alignment is zero.");
                return false;
            }
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
            if (max_payload_align == 0) {
                 set_canon_error(error_buf, error_buf_size, "Result max_payload_align is zero.");
                 return false;
            }
            uint32 payload_offset = align_up(disc_size, max_payload_align);
            *out_size = payload_offset + max_payload_size;
            *out_alignment = disc_align > max_payload_align ? disc_align : max_payload_align;
            if (*out_alignment == 0) {
                set_canon_error(error_buf, error_buf_size, "Result calculated alignment is zero.");
                return false;
            }
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
            if (max_case_payload_align == 0) {
                set_canon_error(error_buf, error_buf_size, "Variant max_case_payload_align is zero.");
                return false;
            }
            uint32 payload_offset = align_up(disc_size, max_case_payload_align);
            *out_size = payload_offset + max_case_payload_size;
            *out_alignment = disc_align > max_case_payload_align ? disc_align : max_case_payload_align;
            if (*out_alignment == 0) {
                set_canon_error(error_buf, error_buf_size, "Variant calculated alignment is zero.");
                return false;
            }
            *out_size = align_up(*out_size, *out_alignment);
            return true;
        }
        case VAL_TYPE_KIND_FLAGS:
        {
            uint32 label_count = val_type->u.flags.label_count;
            if (label_count == 0) {
                *out_size = 0;
                *out_alignment = 1; // Minimum alignment for empty types
            } else {
                // Calculate how many 32-bit integers are needed
                uint32 num_i32s = (label_count + 31) / 32;
                *out_size = num_i32s * sizeof(uint32);
                *out_alignment = sizeof(uint32); // Flags are stored as a sequence of i32s
            }
            return true;
        }

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
    global_resource_table[new_handle].owner_module_inst = NULL; // Initialize destructor fields
    global_resource_table[new_handle].dtor_core_func_idx = (uint32)-1;

    // Retrieve destructor information if available
    // This requires access to the component definition from which this resource type comes.
    // The canonical_def->u.type_idx_op.type_idx is an index into the *current component's* type definitions.
    WASMComponentInstance *current_comp_inst = wasm_runtime_get_component_instance(exec_env);
    if (current_comp_inst && current_comp_inst->component_def) { // current_comp_inst should be WASMComponentInstanceInternal
        WASMComponent *component_def = ((WASMComponentInstanceInternal*)current_comp_inst)->component_def;
        uint32 resource_type_def_idx = canonical_def->u.type_idx_op.type_idx;

        if (resource_type_def_idx < component_def->type_definition_count) {
            WASMComponentDefinedType *defined_type = &component_def->type_definitions[resource_type_def_idx];
            if (defined_type->kind == DEF_TYPE_KIND_RESOURCE) {
                WASMComponentResourceType *res_type_info = &defined_type->u.res_type;
                if (res_type_info->dtor_func_idx != (uint32)-1) {
                    // Assumption: The destructor core function is in the *current* module instance
                    // if this resource.new is called from a core Wasm context.
                    // This is a simplification. A robust solution needs to determine which core module
                    // actually implements this resource type and its destructor.
                    WASMModuleInstance *module_inst = wasm_runtime_get_module_inst(exec_env);
                    if (module_inst) {
                        // Validate dtor_core_func_idx against module_inst's function counts
                        // This index is into the *defining* core module's function space.
                        // If the resource is defined by this module_inst:
                        if (res_type_info->dtor_func_idx < module_inst->function_count) {
                             global_resource_table[new_handle].owner_module_inst = module_inst;
                             global_resource_table[new_handle].dtor_core_func_idx = res_type_info->dtor_func_idx;
                             LOG_VERBOSE("Registered destructor for resource handle %d: mod_inst %p, func_idx %u",
                                        new_handle, module_inst, res_type_info->dtor_func_idx);
                        } else {
                            LOG_WARNING("Resource type %u has dtor_func_idx %u out of bounds for current module %p. Destructor not registered.",
                                        resource_type_def_idx, res_type_info->dtor_func_idx, module_inst);
                        }
                    } else {
                        LOG_WARNING("Cannot register destructor for resource type %u: module_inst not available in exec_env.", resource_type_def_idx);
                    }
                }
            } else {
                 LOG_WARNING("Type definition at index %u is not a resource type, cannot get dtor for new resource.", resource_type_def_idx);
            }
        } else {
            LOG_WARNING("Resource type index %u out of bounds for component type definitions, cannot get dtor for new resource.", resource_type_def_idx);
        }
    } else {
         LOG_WARNING("Component instance or definition not available, cannot resolve destructor for new resource.");
    }


    *(int32_t*)core_value_write_ptr = (int32_t)new_handle;
    LOG_VERBOSE("Created new resource handle %d for component type idx %u", new_handle, global_resource_table[new_handle].component_resource_type_idx);

    // Add to active resource list of the owning component instance
    if (current_comp_inst) { // current_comp_inst is WASMComponentInstance* (effectively internal)
        WASMComponentInstanceInternal *owning_comp_inst_internal = (WASMComponentInstanceInternal*)current_comp_inst;
        ActiveResourceHandle *active_handle_node = loader_malloc(sizeof(ActiveResourceHandle), error_buf, error_buf_size);
        if (!active_handle_node) {
            // Failed to allocate node. This is problematic as the resource is already in global_resource_table.
            // For now, log error and continue. Resource will be cleaned up by deinstantiate if not dropped.
            // Or, we could try to 'undo' the resource creation, but that's complex.
            LOG_ERROR("Failed to allocate ActiveResourceHandle node for handle %d. Resource tracking may be incomplete.", new_handle);
            // No need to set error_buf here as the main operation succeeded.
        } else {
            active_handle_node->resource_type_idx = global_resource_table[new_handle].component_resource_type_idx;
            active_handle_node->resource_handle_core_value = new_handle;
            active_handle_node->next = NULL;

            os_mutex_lock(&owning_comp_inst_internal->active_resource_list_lock);
            active_handle_node->next = owning_comp_inst_internal->active_resource_list_head;
            owning_comp_inst_internal->active_resource_list_head = active_handle_node;
            os_mutex_unlock(&owning_comp_inst_internal->active_resource_list_lock);
            LOG_VERBOSE("Added resource handle %d to active list of component instance %p", new_handle, (void*)owning_comp_inst_internal);
        }
    } else {
        LOG_WARNING("Resource handle %d created, but owning component instance not found. Cannot add to active list.", new_handle);
    }

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
    
    // Call destructor if registered
    WASMModuleInstance *owner_module = global_resource_table[handle].owner_module_inst;
    uint32 dtor_idx = global_resource_table[handle].dtor_core_func_idx;

    if (owner_module && dtor_idx != (uint32)-1) {
        LOG_VERBOSE("Calling destructor for resource handle %d (owner_module: %p, dtor_idx: %u)", handle, owner_module, dtor_idx);
        
        WASMFunctionInstance *dtor_func = NULL;
        // The dtor_idx is an index into the *defining* module's total function space (imports + defined).
        // wasm_runtime_get_function can resolve this.
        dtor_func = wasm_runtime_get_function(owner_module, dtor_idx);

        if (dtor_func) {
            // Destructor signature: (handle: i32) -> ()
            // Check if the signature matches (1 param i32, 0 results) - for robustness.
            // For now, assume it's correct.
            // TODO: Validate destructor signature (param: i32, result: none)

            uint32 argv[1];
            argv[0] = (uint32)handle; // Pass the handle as the argument

            WASMExecEnv *target_exec_env = NULL;
            // Determine the correct exec_env for the call.
            // If the current exec_env's module_inst is the owner_module, we can potentially reuse it.
            // However, creating a temporary one or using a dedicated one for the owner_module is safer.
            // For now, if current exec_env is not for the owner_module, this is complex.
            if (wasm_runtime_get_module_inst(exec_env) == owner_module) {
                target_exec_env = exec_env;
                 if (target_exec_env) { // Check if target_exec_env is valid before calling
                    if (!wasm_runtime_call_wasm(target_exec_env, dtor_func, 1, argv)) {
                        // Exception occurred during destructor call
                        const char *exception = wasm_runtime_get_exception(owner_module);
                        LOG_WARNING("Exception during destructor call for resource handle %d: %s", handle, exception);
                        // Clear the exception on the owner_module.
                        wasm_runtime_clear_exception(owner_module);
                        // Even if destructor traps, the resource is considered dropped from host perspective.
                    }
                    // No need to destroy target_exec_env if it was exec_env
                }
            } else {
                // TODO: Cross-module destructor calls are complex.
                // For now, log a warning and skip the destructor call.
                // The resource will still be marked inactive.
                LOG_WARNING("Cross-module destructor call for resource handle %d (owner %p, current %p) is not supported. Destructor will not be called.",
                            handle, owner_module, wasm_runtime_get_module_inst(exec_env));
                // Do not set target_exec_env, so the call is skipped.
            }
            // The block for `if (target_exec_env)` and its contents are now handled within the if/else above.
        } else {
            LOG_WARNING("Destructor function with index %u not found in owner module for handle %d.", dtor_idx, handle);
        }
    }

    // Mark as inactive and clear dtor info regardless of dtor success/failure
    global_resource_table[handle].is_active = false;
    global_resource_table[handle].host_data = NULL;
    global_resource_table[handle].component_resource_type_idx = 0;
    global_resource_table[handle].owner_module_inst = NULL;
    global_resource_table[handle].dtor_core_func_idx = (uint32)-1;

    LOG_VERBOSE("Dropped resource handle %d", handle);

    // Remove from active resource list of the owning component instance
    WASMComponentInstance *current_comp_inst = wasm_runtime_get_component_instance(exec_env);
    if (owner_module && owner_module->component_inst) {
         // If the resource had an owner_module_inst, it's more reliable to get the component instance from there,
         // as exec_env might be for a different component if there are cross-component calls.
         // However, the active_resource_list is per-component-instance that *owns* the resource's type definition or alias.
         // The current_comp_inst from exec_env is likely the one that initiated the drop.
         // This needs careful consideration if resources are shared or passed between components.
         // For now, assume the resource is dropped by its conceptual owner or within its owning component's context.
         // Using current_comp_inst from exec_env, assuming it's the context of the resource's owner.
    }


    if (current_comp_inst) {
        WASMComponentInstanceInternal *owning_comp_inst_internal = (WASMComponentInstanceInternal*)current_comp_inst;
        os_mutex_lock(&owning_comp_inst_internal->active_resource_list_lock);
        ActiveResourceHandle **p_current = &owning_comp_inst_internal->active_resource_list_head;
        ActiveResourceHandle *found_node = NULL;
        while (*p_current) {
            if ((*p_current)->resource_handle_core_value == (uint32_t)handle) {
                found_node = *p_current;
                *p_current = (*p_current)->next; // Unlink
                break;
            }
            p_current = &(*p_current)->next;
        }
        os_mutex_unlock(&owning_comp_inst_internal->active_resource_list_lock);

        if (found_node) {
            loader_free(found_node);
            LOG_VERBOSE("Removed resource handle %d from active list of component instance %p", handle, (void*)owning_comp_inst_internal);
        } else {
            LOG_WARNING("Resource handle %d was dropped, but not found in active list of component instance %p. May indicate prior cleanup or tracking issue.", handle, (void*)owning_comp_inst_internal);
        }
    } else {
        LOG_WARNING("Resource handle %d dropped, but current component instance not found. Cannot remove from active list.", handle);
    }

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
        case PRIM_VAL_STRING: return VALUE_TYPE_I32; // Strings are (offset, len) pairs, i32 each. This reflects the type of components of the pair.
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
