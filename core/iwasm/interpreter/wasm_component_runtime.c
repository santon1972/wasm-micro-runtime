/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_runtime.h"
#include "../include/wasm_component_loader.h" // Changed to include the new header
#include "wasm_runtime.h"          /* For wasm_instantiate, wasm_deinstantiate */
#include "wasm_loader.h"           /* For wasm_module_destroy (if needed for component def) */
#include "../common/bh_log.h"
#include "../common/bh_platform.h" /* For bh_malloc, bh_free, memset */
#include "wasm_memory.h"        /* For wasm_runtime_validate_app_addr, etc. */


// Helper to set component runtime error messages
static void
set_comp_rt_error(char *error_buf, uint32 error_buf_size, const char *message); // Keep FWD decl

// Forward declarations for recursive calls or new helpers
static bool component_type_compatible(WASMComponentComponentType *expected_comp_type,
                                      WASMComponent *actual_comp_def,
                                      WASMComponent *outer_component_def_context,
                                      char* error_buf, uint32 error_buf_size);
static bool component_func_type_compatible(WASMComponentFuncType *expected_func_type,
                                           WASMComponentFuncType *actual_func_type,
                                           WASMComponent *expected_defining_component,
                                           WASMComponent *actual_defining_component,
                                           char* error_buf, uint32 error_buf_size);
static bool core_global_type_compatible_with_component_val_type(WASMComponentValType *expected_val_type,
                                                                WASMGlobalInstance *actual_core_global,
                                                                WASMComponent *expected_defining_component,
                                                                char* error_buf, uint32 error_buf_size);

// New helper function
static bool
core_component_func_type_compatible_with_core_func_type(WASMComponentCoreFuncType* expected_comp_core_func_type,
                                                        WASMType* actual_core_func_type,
                                                        char* error_buf, uint32 error_buf_size)
{
    if (!expected_comp_core_func_type || !actual_core_func_type) {
        set_comp_rt_error(error_buf, error_buf_size, "NULL function type inputs to core_component_func_type_compatible_with_core_func_type.");
        return false;
    }

    // Compare param counts
    if (expected_comp_core_func_type->param_count != actual_core_func_type->param_count) {
        set_comp_rt_error_v(error_buf, error_buf_size, "Core function type param count mismatch. Expected %d, actual %d.",
                           expected_comp_core_func_type->param_count, actual_core_func_type->param_count);
        return false;
    }

    // Compare param types
    for (uint32 i = 0; i < expected_comp_core_func_type->param_count; ++i) {
        if (expected_comp_core_func_type->param_types[i] != actual_core_func_type->types[i]) {
            set_comp_rt_error_v(error_buf, error_buf_size, "Core function type param type mismatch at index %d. Expected 0x%02X, actual 0x%02X.",
                               i, expected_comp_core_func_type->param_types[i], actual_core_func_type->types[i]);
            return false;
        }
    }

    // Compare result counts
    if (expected_comp_core_func_type->result_count != actual_core_func_type->result_count) {
        set_comp_rt_error_v(error_buf, error_buf_size, "Core function type result count mismatch. Expected %d, actual %d.",
                           expected_comp_core_func_type->result_count, actual_core_func_type->result_count);
        return false;
    }

    // Compare result types
    // Actual core func type stores results after params in the same `types` array.
    uint32 actual_core_result_type_offset = actual_core_func_type->param_count;
    for (uint32 i = 0; i < expected_comp_core_func_type->result_count; ++i) {
        if (expected_comp_core_func_type->result_types[i] != actual_core_func_type->types[actual_core_result_type_offset + i]) {
            set_comp_rt_error_v(error_buf, error_buf_size, "Core function type result type mismatch at index %d. Expected 0x%02X, actual 0x%02X.",
                               i, expected_comp_core_func_type->result_types[i], actual_core_func_type->types[actual_core_result_type_offset + i]);
            return false;
        }
    }
    return true;
}

// Implementation for core module type compatibility
static bool
core_module_type_compatible(WASMComponentCoreModuleType *expected_cmt,
                            WASMModuleInstance *actual_core_module_inst,
                            WASMComponent *defining_component_context, // Context for expected_cmt's type_idx resolution
                            char *error_buf, uint32 error_buf_size)
{
    uint32 i, k;
    WASMModule *actual_module = actual_core_module_inst->module;

    // Check imports
    for (i = 0; i < expected_cmt->import_count; ++i) {
        WASMComponentCoreModuleImport *expected_import = &expected_cmt->imports[i];
        WASMImport *actual_import = NULL;

        for (k = 0; k < actual_module->import_count; ++k) {
            if (strcmp(actual_module->imports[k].module_name, expected_import->module_name) == 0 &&
                strcmp(actual_module->imports[k].field_name, expected_import->field_name) == 0) {
                actual_import = &actual_module->imports[k];
                break;
            }
        }

        if (!actual_import) {
            set_comp_rt_error_v(error_buf, error_buf_size, "Expected core module import '%s':'%s' not found in actual module.",
                               expected_import->module_name, expected_import->field_name);
            return false;
        }

        if (expected_import->kind != actual_import->kind) {
            set_comp_rt_error_v(error_buf, error_buf_size, "Core module import '%s':'%s' kind mismatch. Expected %d, actual %d.",
                               expected_import->module_name, expected_import->field_name, expected_import->kind, actual_import->kind);
            return false;
        }

        switch (expected_import->kind) {
            case WASM_IMPORT_KIND_FUNC:
            {
                // Ensure type_idx is valid for the defining_component_context's core_types array
                if (expected_import->type_idx >= defining_component_context->core_type_count) {
                     set_comp_rt_error_v(error_buf, error_buf_size, "Invalid type_idx %u for expected core func import '%s':'%s' (core_type_count %u).",
                                        expected_import->type_idx, expected_import->module_name, expected_import->field_name, defining_component_context->core_type_count);
                     return false;
                }
                // Ensure the type at type_idx is actually a core function type
                if (defining_component_context->core_types[expected_import->type_idx].kind != WASM_COMPONENT_CORE_FUNC_TYPE_KIND) {
                     set_comp_rt_error_v(error_buf, error_buf_size, "Type at type_idx %u for expected core func import '%s':'%s' is not a core func type (kind %u).",
                                        expected_import->type_idx, expected_import->module_name, expected_import->field_name,
                                        defining_component_context->core_types[expected_import->type_idx].kind);
                     return false;
                }
                WASMComponentCoreFuncType *expected_core_func_type = &defining_component_context->core_types[expected_import->type_idx].u.core_type_def.func_type;
                if (!core_component_func_type_compatible_with_core_func_type(expected_core_func_type, actual_import->u.function.func_type, error_buf, error_buf_size)) {
                   return false;
                }
                break;
            }
            case WASM_IMPORT_KIND_TABLE:
                // Compare actual_import->u.table with expected type
                // Expected type needs to be resolved from expected_import->type_idx if it pointed to a table type def
                LOG_TODO("Detailed type check for imported core table in core_module_type_compatible.");
                break;
            case WASM_IMPORT_KIND_MEMORY:
                LOG_TODO("Detailed type check for imported core memory in core_module_type_compatible.");
                break;
            case WASM_IMPORT_KIND_GLOBAL:
                LOG_TODO("Detailed type check for imported core global in core_module_type_compatible.");
                break;
            default:
                set_comp_rt_error_v(error_buf, error_buf_size, "Unsupported import kind %d for core module type compatibility.", expected_import->kind);
                return false;
        }
    }

    // Check exports
    for (i = 0; i < expected_cmt->export_count; ++i) {
        WASMComponentCoreModuleExport *expected_export = &expected_cmt->exports[i];
        bool found_export = false;

        switch (expected_export->kind) {
            case WASM_EXPORT_KIND_FUNC:
            {
                for (k = 0; k < actual_module_inst->export_func_count; ++k) {
                    if (strcmp(actual_module_inst->export_functions[k].name, expected_export->name) == 0) {
                        if (expected_export->type_idx >= defining_component_context->core_type_count ||
                            defining_component_context->core_types[expected_export->type_idx].kind != WASM_COMPONENT_CORE_FUNC_TYPE_KIND) {
                            set_comp_rt_error_v(error_buf, error_buf_size, "Invalid type_idx %d for expected core func export '%s'.",
                                                expected_export->type_idx, expected_export->name);
                            return false;
                        }
                        WASMComponentCoreFuncType* expected_core_func_type = &defining_component_context->core_types[expected_export->type_idx].u.core_type_def.func_type;
                        WASMFunctionInstance* actual_func_inst = actual_module_inst->export_functions[k].function;
                        if (!core_component_func_type_compatible_with_core_func_type(expected_core_func_type, actual_func_inst->u.func.func_type_linked, error_buf, error_buf_size)) {
                           return false;
                        }
                        found_export = true;
                        break;
                    }
                }
                break;
            }
            case WASM_EXPORT_KIND_TABLE:
                 for (k = 0; k < actual_module_inst->export_table_count; ++k) {
                    if (strcmp(actual_module_inst->export_tables[k].name, expected_export->name) == 0) {
                        LOG_TODO("Detailed type check for exported core table in core_module_type_compatible.");
                        found_export = true;
                        break;
                    }
                }
                break;
            case WASM_EXPORT_KIND_MEMORY:
                for (k = 0; k < actual_module_inst->export_memory_count; ++k) {
                    if (strcmp(actual_module_inst->export_memories[k].name, expected_export->name) == 0) {
                        LOG_TODO("Detailed type check for exported core memory in core_module_type_compatible.");
                        found_export = true;
                        break;
                    }
                }
                break;
            case WASM_EXPORT_KIND_GLOBAL:
                for (k = 0; k < actual_module_inst->export_global_count; ++k) {
                    if (strcmp(actual_module_inst->export_globals[k].name, expected_export->name) == 0) {
                        LOG_TODO("Detailed type check for exported core global in core_module_type_compatible.");
                        found_export = true;
                        break;
                    }
                }
                break;
            default:
                set_comp_rt_error_v(error_buf, error_buf_size, "Unsupported export kind %d for core module type compatibility.", expected_export->kind);
                return false;
        }
        if (!found_export) {
            set_comp_rt_error_v(error_buf, error_buf_size, "Expected core module export '%s' (kind %d) not found in actual module instance.",
                               expected_export->name, expected_export->kind);
            return false;
        }
    }

    return true;
}

// Static helper functions for finding exported items in a module instance
static WASMFunctionInstance *
find_exported_function_instance(WASMModuleInstance *module_inst, const char *name,
                                char *error_buf, uint32 error_buf_size)
{
    for (uint32 i = 0; i < module_inst->export_func_count; ++i) {
        if (strcmp(module_inst->export_functions[i].name, name) == 0) {
            return module_inst->export_functions[i].function;
        }
    }
    set_comp_rt_error_v(error_buf, error_buf_size, "Function export '%s' not found in source instance.", name);
    return NULL;
}

static WASMGlobalInstance *
find_exported_global_instance(WASMModuleInstance *module_inst, const char *name, bool is_mutable,
                                char *error_buf, uint32 error_buf_size)
{
    for (uint32 i = 0; i < module_inst->export_global_count; ++i) {
        if (strcmp(module_inst->export_globals[i].name, name) == 0) {
            // Mutability check is done by the caller, e.g. in wasm_component_instance_instantiate
            return module_inst->export_globals[i].global;
        }
    }
    set_comp_rt_error_v(error_buf, error_buf_size, "Global export '%s' not found in source instance.", name);
    return NULL;
}

static WASMTableInstance *
find_exported_table_instance(WASMModuleInstance *module_inst, const char *name,
                               char *error_buf, uint32 error_buf_size)
{
     for (uint32 i = 0; i < module_inst->export_table_count; ++i) {
        if (strcmp(module_inst->export_tables[i].name, name) == 0) {
            return module_inst->export_tables[i].table;
        }
    }
    set_comp_rt_error_v(error_buf, error_buf_size, "Table export '%s' not found in source instance.", name);
    return NULL;
}

static WASMMemoryInstance *
find_exported_memory_instance(WASMModuleInstance *module_inst, const char *name,
                                char *error_buf, uint32 error_buf_size)
{
    for (uint32 i = 0; i < module_inst->export_memory_count; ++i) {
        if (strcmp(module_inst->export_memories[i].name, name) == 0) {
            return module_inst->export_memories[i].memory;
        }
    }
    set_comp_rt_error_v(error_buf, error_buf_size, "Memory export '%s' not found in source instance.", name);
    return NULL;
}


static void
set_comp_rt_error(char *error_buf, uint32 error_buf_size, const char *message)
{
    if (error_buf) {
        snprintf(error_buf, error_buf_size, "Component Runtime Error: %s", message);
    }
}

static void
set_comp_rt_error_v(char *error_buf, uint32 error_buf_size, const char *format, ...)
{
    va_list args;
    char buf[128];

    if (error_buf) {
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        snprintf(error_buf, error_buf_size, "Component Runtime Error: %s", buf);
    }
}


WASMComponentInstanceInternal *
wasm_component_instance_instantiate(
    WASMComponent *component,
    WASMExecEnv *parent_exec_env,
    ResolvedComponentImportItem *resolved_imports,
    uint32 num_resolved_imports,
    char *error_buf,
    uint32 error_buf_size)
{
    WASMComponentInstanceInternal *comp_inst_internal = NULL;
    uint32 i;

    if (!component) {
        set_comp_rt_error(error_buf, error_buf_size, "Input component definition is NULL.");
        return NULL;
    }

    comp_inst_internal = bh_malloc(sizeof(WASMComponentInstanceInternal));
    if (!comp_inst_internal) {
        set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate memory for component instance.");
        return NULL;
    }
    memset(comp_inst_internal, 0, sizeof(WASMComponentInstanceInternal));

    comp_inst_internal->component_def = component;
    comp_inst_internal->resolved_imports = resolved_imports; // Shallow copy
    comp_inst_internal->num_resolved_imports = num_resolved_imports;

    // Count how many core modules will be truly instantiated by this component
    // (i.e., kind CORE_INSTANCE_KIND_INSTANTIATE)
    uint32 num_modules_to_instantiate = 0;
    for (i = 0; i < component->core_instance_count; ++i) {
        if (component->core_instances[i].kind == CORE_INSTANCE_KIND_INSTANTIATE) {
            num_modules_to_instantiate++;
        }
    }
    comp_inst_internal->num_module_instances = num_modules_to_instantiate;
    if (num_modules_to_instantiate > 0) {
        comp_inst_internal->module_instances = bh_malloc(num_modules_to_instantiate * sizeof(WASMModuleInstance*));
        if (!comp_inst_internal->module_instances) {
            set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate memory for module instances array.");
            bh_free(comp_inst_internal);
            return NULL;
        }
        memset(comp_inst_internal->module_instances, 0, num_modules_to_instantiate * sizeof(WASMModuleInstance*));
    }

    // Count how many nested components will be truly instantiated
    uint32 num_nested_comps_to_instantiate = 0;
    for (i = 0; i < component->component_instance_count; ++i) {
        if (component->component_instances[i].kind == COMPONENT_INSTANCE_KIND_INSTANTIATE) {
            num_nested_comps_to_instantiate++;
        }
    }
    comp_inst_internal->num_component_instances = num_nested_comps_to_instantiate;

    if (num_nested_comps_to_instantiate > 0) {
        comp_inst_internal->component_instances = bh_malloc(num_nested_comps_to_instantiate * sizeof(WASMComponentInstanceInternal*));
        if (!comp_inst_internal->component_instances) {
            set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate memory for nested component instances array.");
            if (comp_inst_internal->module_instances) {
                bh_free(comp_inst_internal->module_instances);
            }
            bh_free(comp_inst_internal);
            return NULL;
        }
        memset(comp_inst_internal->component_instances, 0, num_nested_comps_to_instantiate * sizeof(WASMComponentInstanceInternal*));
    }

    // Map definition-time core_instance_idx to runtime module_instances array index
    // This is needed because module_instances only stores truly instantiated modules.
    uint32 *core_instance_to_runtime_module_idx_map = NULL;
    if (component->core_instance_count > 0) {
        core_instance_to_runtime_module_idx_map = bh_malloc(component->core_instance_count * sizeof(uint32));
        if (!core_instance_to_runtime_module_idx_map) {
             set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate memory for core instance map.");
             // Free previously allocated memory
             if (comp_inst_internal->module_instances) bh_free(comp_inst_internal->module_instances);
             if (comp_inst_internal->component_instances) bh_free(comp_inst_internal->component_instances);
             bh_free(comp_inst_internal);
             return NULL;
        }
        // Initialize with a sentinel or handle appropriately if an index is used before its module is instantiated.
        // For now, this map will be filled as modules are instantiated.
    comp_inst_internal->core_instance_map = core_instance_to_runtime_module_idx_map;
    }


    LOG_DEBUG("Component instance structure allocated. Starting core module instantiation.");
    uint32 current_runtime_module_idx = 0;

    for (i = 0; i < component->core_instance_count; ++i) {
        WASMComponentCoreInstance *core_instance_def = &component->core_instances[i];

        if (core_instance_def->kind == CORE_INSTANCE_KIND_INSTANTIATE) {
            uint32 module_def_idx = core_instance_def->u.instantiate.module_idx;
            WASMModule *wasm_module = component->core_modules[module_def_idx];
            WASMModuleInstance *new_module_inst = NULL;
            
            uint32 num_inst_args = core_instance_def->u.instantiate.arg_count;
            WASMComponentCoreInstanceArg *inst_args = core_instance_def->u.instantiate.args;

            RuntimeImportedFunc *resolved_func_imports = NULL;
            RuntimeImportedGlobal *resolved_global_imports = NULL;
            RuntimeImportedTable *resolved_table_imports = NULL;
            RuntimeImportedMemory *resolved_memory_imports = NULL;
            
            bool import_resolution_failed = false;

            // Allocate arrays for resolved imports based on the module's needs
            if (wasm_module->import_function_count > 0) {
                resolved_func_imports = bh_malloc(wasm_module->import_function_count * sizeof(RuntimeImportedFunc));
                if (!resolved_func_imports) { import_resolution_failed = true; goto import_resolution_done; }
                memset(resolved_func_imports, 0, wasm_module->import_function_count * sizeof(RuntimeImportedFunc));
            }
            if (wasm_module->import_global_count > 0) {
                resolved_global_imports = bh_malloc(wasm_module->import_global_count * sizeof(RuntimeImportedGlobal));
                if (!resolved_global_imports) { import_resolution_failed = true; goto import_resolution_done; }
                memset(resolved_global_imports, 0, wasm_module->import_global_count * sizeof(RuntimeImportedGlobal));
            }
            if (wasm_module->import_table_count > 0) {
                resolved_table_imports = bh_malloc(wasm_module->import_table_count * sizeof(RuntimeImportedTable));
                if (!resolved_table_imports) { import_resolution_failed = true; goto import_resolution_done; }
                memset(resolved_table_imports, 0, wasm_module->import_table_count * sizeof(RuntimeImportedTable));
            }
            if (wasm_module->import_memory_count > 0) {
                resolved_memory_imports = bh_malloc(wasm_module->import_memory_count * sizeof(RuntimeImportedMemory));
                if (!resolved_memory_imports) { import_resolution_failed = true; goto import_resolution_done; }
                memset(resolved_memory_imports, 0, wasm_module->import_memory_count * sizeof(RuntimeImportedMemory));
            }

        import_resolution_done:
            if (import_resolution_failed) {
                set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate memory for resolved imports arrays.");
                if (resolved_func_imports) bh_free(resolved_func_imports);
                if (resolved_global_imports) bh_free(resolved_global_imports);
                if (resolved_table_imports) bh_free(resolved_table_imports);
                if (resolved_memory_imports) bh_free(resolved_memory_imports);
                
                // Cleanup main allocations before returning
                if (comp_inst_internal->core_instance_map) bh_free(comp_inst_internal->core_instance_map);
                for (uint32 j = 0; j < current_runtime_module_idx; ++j) { /* ... (deinstantiate modules) ... */ }
                if (comp_inst_internal->module_instances) bh_free(comp_inst_internal->module_instances);
                if (comp_inst_internal->component_instances) bh_free(comp_inst_internal->component_instances);
                bh_free(comp_inst_internal);
                return NULL;
            }
            
            uint32 current_func_import_k = 0;
            uint32 current_global_import_k = 0;
            uint32 current_table_import_k = 0;
            uint32 current_memory_import_k = 0;

            LOG_VERBOSE("Resolving imports for core module definition %u (instance def %u)", module_def_idx, i);
            for (uint32 import_k = 0; import_k < wasm_module->import_count; ++import_k) {
                WASMImport *import_def = &wasm_module->imports[import_k];
                WASMComponentCoreInstanceArg *matched_arg = NULL;

                // Find the instantiation argument that satisfies this import.
                // The matching is based on the export name provided in the arg list.
                for (uint32 arg_k = 0; arg_k < num_inst_args; ++arg_k) {
                    if (strcmp(inst_args[arg_k].name, import_def->field_name) == 0) {
                        // Check kind compatibility
                        // WASMImportKind vs WASMComponentExternKind (from matched_arg->kind)
                        // The matched_arg->kind is already a WASMComponentExternKind populated by the loader.
                        bool kind_compatible = false;
                        switch (import_def->kind) {
                            case WASM_IMPORT_KIND_FUNC:
                                kind_compatible = (matched_arg->kind == COMPONENT_ITEM_KIND_FUNC);
                                break;
                            case WASM_IMPORT_KIND_TABLE:
                                kind_compatible = (matched_arg->kind == COMPONENT_ITEM_KIND_TABLE);
                                break;
                            case WASM_IMPORT_KIND_MEMORY:
                                kind_compatible = (matched_arg->kind == COMPONENT_ITEM_KIND_MEMORY);
                                break;
                            case WASM_IMPORT_KIND_GLOBAL:
                                kind_compatible = (matched_arg->kind == COMPONENT_ITEM_KIND_GLOBAL);
                                break;
                            default: // Event / not standard Wasm core kinds
                                kind_compatible = false;
                                break;
                        }
                        if (kind_compatible) {
                            matched_arg = &inst_args[arg_k];
                            break;
                        } else {
                            LOG_VERBOSE("Import '%s':'%s' (kind %u) not satisfied by arg '%s' (kind %u) due to kind mismatch.",
                                       import_def->module_name, import_def->field_name, import_def->kind,
                                       inst_args[arg_k].name, matched_arg->kind);
                            // Continue searching other args, maybe another arg with same name has correct kind
                        }
                    }
                }

                if (!matched_arg) {
                    set_comp_rt_error_v(error_buf, error_buf_size,
                                       "Import '%s':'%s' for module def %u (instance %u) not satisfied by any instantiation argument.",
                                       import_def->module_name, import_def->field_name, module_def_idx, i);
                    import_resolution_failed = true;
                    break;
                }

                uint32 src_core_inst_def_idx = matched_arg->instance_idx;
                uint32 src_runtime_mod_arr_idx = comp_inst_internal->core_instance_map[src_core_inst_def_idx];

                if (src_runtime_mod_arr_idx == (uint32)-1) {
                    // Source is CORE_INSTANCE_KIND_INLINE_EXPORT or similar.
                    // Attempt to resolve from the component's own resolved imports.
                    // This handles the case where an inline export group re-exports a host-provided import.
                    // matched_arg->name is the export name from the inline group, which should match import_def->field_name.
                    bool found_in_comp_imports = false;
                    for (uint32 k = 0; k < comp_inst_internal->num_resolved_imports; ++k) {
                        if (strcmp(comp_inst_internal->resolved_imports[k].name, import_def->field_name) == 0) {
                            // Check kind compatibility between the component import and the core module import
                            bool kind_match = false;
                            switch(import_def->kind) {
                                case WASM_IMPORT_KIND_FUNC: kind_match = (comp_inst_internal->resolved_imports[k].kind == COMPONENT_ITEM_KIND_FUNC); break;
                                case WASM_IMPORT_KIND_GLOBAL: kind_match = (comp_inst_internal->resolved_imports[k].kind == COMPONENT_ITEM_KIND_GLOBAL); break;
                                case WASM_IMPORT_KIND_MEMORY: kind_match = (comp_inst_internal->resolved_imports[k].kind == COMPONENT_ITEM_KIND_MEMORY); break;
                                case WASM_IMPORT_KIND_TABLE: kind_match = (comp_inst_internal->resolved_imports[k].kind == COMPONENT_ITEM_KIND_TABLE); break;
                                default: break;
                            }
                            if (kind_match) {
                                switch (import_def->kind) {
                                    case WASM_IMPORT_KIND_FUNC:
                                        // Assuming the resolved import item.function is already a WASMFunctionInstance* if it's a core function
                                        // This might be a host function, which could also be represented by WASMFunctionInstance
                                        resolved_func_imports[current_func_import_k].module_name = (char*)import_def->module_name;
                                        resolved_func_imports[current_func_import_k].field_name = (char*)import_def->field_name;
                                        resolved_func_imports[current_func_import_k].func_ptr_linked = comp_inst_internal->resolved_imports[k].item.function;
                                        resolved_func_imports[current_func_import_k].signature = import_def->u.function.func_type;
                                        // If item.function is a WASMFunctionInstance, its is_native and call_conv_raw should be set.
                                        if (comp_inst_internal->resolved_imports[k].item.function) {
                                            WASMFunctionInstance *resolved_func = (WASMFunctionInstance *)comp_inst_internal->resolved_imports[k].item.function;
                                        // Type check for functions
                                        WASMFunctionInstance *resolved_func_inst = (WASMFunctionInstance *)comp_inst_internal->resolved_imports[k].item.function;
                                        if (!resolved_func_inst) { /* Should not happen if kind matches */
                                            set_comp_rt_error_v(error_buf, error_buf_size, "Internal error: Function import '%s' resolved item is NULL.", import_def->field_name);
                                                import_resolution_failed = true; break;
                                            }
                                        // Get the WASMType* from the resolved function.
                                        // For host functions, func_type_linked is usually set.
                                        // For Wasm functions, it's also func_type_linked.
                                        WASMType *resolved_type = resolved_func_inst->u.func.func_type_linked;
                                        if (!resolved_type) { // Fallback for older host function style or if not populated
                                             resolved_type = resolved_func_inst->type;
                                        }

                                        if (!wasm_type_compatible(import_def->u.function.func_type, resolved_type)) {
                                            set_comp_rt_error_v(error_buf, error_buf_size, "Function import '%s' (from component import) type signature mismatch.", import_def->field_name);
                                                import_resolution_failed = true; break;
                                            }

                                        resolved_func_imports[current_func_import_k].module_name = (char*)import_def->module_name;
                                        resolved_func_imports[current_func_import_k].field_name = (char*)import_def->field_name;
                                        resolved_func_imports[current_func_import_k].func_ptr_linked = resolved_func_inst;
                                        resolved_func_imports[current_func_import_k].signature = import_def->u.function.func_type;
                                        resolved_func_imports[current_func_import_k].is_native_func = resolved_func_inst->is_native_func;
                                        resolved_func_imports[current_func_import_k].call_conv_raw = resolved_func_inst->call_conv_raw;
                                        current_func_import_k++;
                                        break;
                                    case WASM_IMPORT_KIND_GLOBAL:
                                        {
                                            WASMGlobalInstance *resolved_global = comp_inst_internal->resolved_imports[k].item.global;
                                            if (resolved_global->type != import_def->u.global.type) {
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Global import '%s' (from component import) type mismatch. Expected %d, got %d.",
                                                                   import_def->field_name, import_def->u.global.type, resolved_global->type);
                                                import_resolution_failed = true; break;
                                            }
                                            if (resolved_global->is_mutable != import_def->u.global.is_mutable) {
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Global import '%s' (from component import) mutability mismatch. Expected %d, got %d.",
                                                                   import_def->field_name, import_def->u.global.is_mutable, resolved_global->is_mutable);
                                                import_resolution_failed = true; break;
                                            }
                                            resolved_global_imports[current_global_import_k].module_name = (char*)import_def->module_name;
                                            resolved_global_imports[current_global_import_k].field_name = (char*)import_def->field_name;
                                            resolved_global_imports[current_global_import_k].global_ptr_linked = resolved_global;
                                            resolved_global_imports[current_global_import_k].is_linked = true;
                                            current_global_import_k++;
                                            break;
                                            }
                                    case WASM_IMPORT_KIND_TABLE:
                                        {
                                            WASMTableInstance *resolved_table = comp_inst_internal->resolved_imports[k].item.table;
                                            // WASMImport *import_def has u.table which is WASMTableImport
                                            // WASMTableImport has elem_type, init_size, max_size, has_max_size
                                            if (import_def->u.table.elem_type != resolved_table->elem_type) {
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' (from component import) element type mismatch. Expected %d, got %d.",
                                                                   import_def->field_name, import_def->u.table.elem_type, resolved_table->elem_type);
                                                import_resolution_failed = true; break;
                                            }
                                            if (resolved_table->init_size < import_def->u.table.init_size) {
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' (from component import) initial size too small. Expected >=%u, got %u.",
                                                                   import_def->field_name, import_def->u.table.init_size, resolved_table->init_size);
                                                import_resolution_failed = true; break;
                                            }
                                            if (import_def->u.table.has_max_size) {
                                                if (!resolved_table->has_max_size) { // resolved_table->max_size == 0 means no max if has_max_size is false
                                                    set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' (from component import) expects max size, but export has no max.", import_def->field_name);
                                                    import_resolution_failed = true; break;
                                                }
                                                if (resolved_table->max_size > import_def->u.table.max_size) {
                                                    set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' (from component import) max size too large. Expected <=%u, got %u.",
                                                                       import_def->field_name, import_def->u.table.max_size, resolved_table->max_size);
                                                    import_resolution_failed = true; break;
                                                }
                                            }
                                            resolved_table_imports[current_table_import_k].module_name = (char*)import_def->module_name;
                                            resolved_table_imports[current_table_import_k].field_name = (char*)import_def->field_name;
                                            resolved_table_imports[current_table_import_k].table_inst_linked = resolved_table;
                                            current_table_import_k++;
                                            break;
                                            }
                                    case WASM_IMPORT_KIND_MEMORY:
                                        {
                                            WASMMemoryInstance *resolved_memory = comp_inst_internal->resolved_imports[k].item.memory;
                                            // WASMImport *import_def has u.memory which is WASMMemoryImport
                                            // WASMMemoryImport has init_page_count, max_page_count, has_max_size, is_shared
                                            if (resolved_memory->init_page_count < import_def->u.memory.init_page_count) {
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' (from component import) initial pages too small. Expected >=%u, got %u.",
                                                                   import_def->field_name, import_def->u.memory.init_page_count, resolved_memory->init_page_count);
                                                import_resolution_failed = true; break;
                                            }
                                            if (import_def->u.memory.has_max_size) {
                                                if (resolved_memory->max_page_count == 0) { // resolved_memory->max_page_count is 0 if no max for WASMMemoryInstance
                                                    set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' (from component import) expects max pages, but export has no max.", import_def->field_name);
                                                    import_resolution_failed = true; break;
                                                }
                                                if (resolved_memory->max_page_count > import_def->u.memory.max_page_count) {
                                                    set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' (from component import) max pages too large. Expected <=%u, got %u.",
                                                                       import_def->field_name, import_def->u.memory.max_page_count, resolved_memory->max_page_count);
                                                    import_resolution_failed = true; break;
                                                }
                                            }
                                            if (import_def->u.memory.is_shared != resolved_memory->is_shared) {
                                                 set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' (from component import) shared flag mismatch. Expected %d, got %d.",
                                                                   import_def->field_name, import_def->u.memory.is_shared, resolved_memory->is_shared);
                                                import_resolution_failed = true; break;
                                            }
                                            resolved_memory_imports[current_memory_import_k].module_name = (char*)import_def->module_name;
                                            resolved_memory_imports[current_memory_import_k].field_name = (char*)import_def->field_name;
                                            resolved_memory_imports[current_memory_import_k].memory_inst_linked = resolved_memory;
                                        current_memory_import_k++;
                                        break;
                                    }
                                    default:
                                        set_comp_rt_error_v(error_buf, error_buf_size, "Import '%s': kind %d from component import not yet fully supported for inline export.", import_def->field_name, import_def->kind);
                                        import_resolution_failed = true; break;
                                }
                                found_in_comp_imports = true;
                                break;
                            }
                        }
                    }
                    if (!found_in_comp_imports && !import_resolution_failed) {
                        set_comp_rt_error_v(error_buf, error_buf_size,
                                           "Import source for '%s' (module def %u) is inline export, but no matching component import found or kind mismatch.",
                                           import_def->field_name, module_def_idx);
                        import_resolution_failed = true;
                    }
                    // If import_resolution_failed, the outer loop `if (import_resolution_failed) break;` will catch it.
                    // If resolved, continue to the next import_k.
                    if (found_in_comp_imports || import_resolution_failed) {
                         if (import_resolution_failed) break; // from switch or if not found after loop
                         else continue; // Resolved from component import, go to next import_k
                    }
                }

                WASMModuleInstance *src_mod_inst = comp_inst_internal->module_instances[src_runtime_mod_arr_idx];
                if (!src_mod_inst) { // Should not happen if map and instantiation order is correct
                    set_comp_rt_error_v(error_buf, error_buf_size,
                                       "Internal error: Source module instance for import '%s' (module def %u) is NULL.",
                                       import_def->field_name, module_def_idx);
                    import_resolution_failed = true;
                    break;
                }

                // Now, find the export in src_mod_inst by matched_arg->name (which is the export name from source)
                // and import_def->kind, then populate the resolved_xxx_imports array.
                const char *export_name_from_arg = matched_arg->name;

                switch (import_def->kind) {
                    case WASM_IMPORT_KIND_FUNC:
                    {
                        WASMFunctionInstance *func_inst = find_exported_function_instance(src_mod_inst, export_name_from_arg, error_buf, error_buf_size);
                        if (!func_inst) { import_resolution_failed = true; break; }
                        
                        resolved_func_imports[current_func_import_k].module_name = (char*)import_def->module_name; // casting away const, but runtime doesn't modify
                        resolved_func_imports[current_func_import_k].field_name = (char*)import_def->field_name;   // casting away const
                        resolved_func_imports[current_func_import_k].func_ptr_linked = func_inst;
                        resolved_func_imports[current_func_import_k].signature = import_def->u.function.func_type;
                        resolved_func_imports[current_func_import_k].call_conv_raw = false; 
                        resolved_func_imports[current_func_import_k].attachment = NULL; 
                        resolved_func_imports[current_func_import_k].is_native_func = false;
                        current_func_import_k++;
                        break;
                    }
                    case WASM_IMPORT_KIND_GLOBAL:
                    {
                        WASMGlobalInstance *global_inst = find_exported_global_instance(src_mod_inst, export_name_from_arg, 
                                                                                        import_def->u.global.is_mutable, /* Pass expected mutability */
                                                                                        error_buf, error_buf_size);
                        if (!global_inst) { import_resolution_failed = true; break; }
                        if (global_inst->is_mutable != import_def->u.global.is_mutable) {
                             set_comp_rt_error_v(error_buf, error_buf_size, "Global import '%s' mutability mismatch.", import_def->field_name);
                             import_resolution_failed = true; break;
                        }
                        if (global_inst->type != import_def->u.global.type) {
                             set_comp_rt_error_v(error_buf, error_buf_size, "Global import '%s' type mismatch.", import_def->field_name);
                             import_resolution_failed = true; break;
                        }
                        resolved_global_imports[current_global_import_k].module_name = (char*)import_def->module_name;
                        resolved_global_imports[current_global_import_k].field_name = (char*)import_def->field_name;
                        resolved_global_imports[current_global_import_k].global_ptr_linked = global_inst;
                        resolved_global_imports[current_global_import_k].is_linked = true; // Mark as linked
                        current_global_import_k++;
                        break;
                    }
                    case WASM_IMPORT_KIND_TABLE:
                    {
                        WASMTableInstance *table_inst = find_exported_table_instance(src_mod_inst, export_name_from_arg, error_buf, error_buf_size);
                        if (!table_inst) { import_resolution_failed = true; break; }

                        if (import_def->u.table.elem_type != table_inst->elem_type) {
                            set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' element type mismatch (expected %u, got %u).",
                                               import_def->field_name, import_def->u.table.elem_type, table_inst->elem_type);
                            import_resolution_failed = true; break;
                        }
                        if (table_inst->init_size < import_def->u.table.init_size) {
                            set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' initial size too small (need %u, got %u).",
                                               import_def->field_name, import_def->u.table.init_size, table_inst->init_size);
                            import_resolution_failed = true; break;
                        }
                        if (import_def->u.table.has_max_size) {
                            if (!table_inst->has_max_size) {
                                set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' expects max size, but export has no max.", import_def->field_name);
                                import_resolution_failed = true; break;
                            }
                            if (table_inst->max_size > import_def->u.table.max_size) {
                                set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' max size too large (need <= %u, got %u).",
                                                   import_def->field_name, import_def->u.table.max_size, table_inst->max_size);
                                import_resolution_failed = true; break;
                            }
                        }
                        // Else (import has no max_size), export can have a max_size or not. This is compatible.

                        resolved_table_imports[current_table_import_k].module_name = (char*)import_def->module_name;
                        resolved_table_imports[current_table_import_k].field_name = (char*)import_def->field_name;
                        resolved_table_imports[current_table_import_k].table_inst_linked = table_inst;
                        current_table_import_k++;
                        break;
                    }
                    case WASM_IMPORT_KIND_MEMORY:
                    {
                        WASMMemoryInstance *memory_inst = find_exported_memory_instance(src_mod_inst, export_name_from_arg, error_buf, error_buf_size);
                        if (!memory_inst) { import_resolution_failed = true; break; }

                        if (memory_inst->init_page_count < import_def->u.memory.init_page_count) {
                            set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' initial pages too small (need %u, got %u).",
                                               import_def->field_name, import_def->u.memory.init_page_count, memory_inst->init_page_count);
                            import_resolution_failed = true; break;
                        }
                        if (import_def->u.memory.has_max_size) { // Check if import expects a max
                            if (memory_inst->max_page_count == 0) { // Export has no max (max_page_count is 0 if no max for WASMMemoryInstance)
                                set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' expects max pages, but export has no max.", import_def->field_name);
                                import_resolution_failed = true; break;
                            }
                            if (memory_inst->max_page_count > import_def->u.memory.max_page_count) {
                                set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' max pages too large (need <= %u, got %u).",
                                                   import_def->field_name, import_def->u.memory.max_page_count, memory_inst->max_page_count);
                                import_resolution_failed = true; break;
                            }
                        }
                        // Else (import has no max_size), export can have a max_size or not. This is compatible.
                        // Also, check shared memory flag if/when component model supports shared memories.
                        // if (import_def->u.memory.is_shared != memory_inst->is_shared) { ... }

                        resolved_memory_imports[current_memory_import_k].module_name = (char*)import_def->module_name;
                        resolved_memory_imports[current_memory_import_k].field_name = (char*)import_def->field_name;
                        resolved_memory_imports[current_memory_import_k].memory_inst_linked = memory_inst;
                        current_memory_import_k++;
                        break;
                    }
                    default:
                        set_comp_rt_error_v(error_buf, error_buf_size, "Unknown import kind %d for '%s':'%s'",
                                           import_def->kind, import_def->module_name, import_def->field_name);
                        import_resolution_failed = true;
                        break;
                }
                if (import_resolution_failed) break;
            } // End loop over imports

            if (import_resolution_failed) {
                 // Error message should already be set.
                if (resolved_func_imports) bh_free(resolved_func_imports); resolved_func_imports = NULL;
                if (resolved_global_imports) bh_free(resolved_global_imports); resolved_global_imports = NULL;
                if (resolved_table_imports) bh_free(resolved_table_imports); resolved_table_imports = NULL;
                if (resolved_memory_imports) bh_free(resolved_memory_imports); resolved_memory_imports = NULL;
                
                // Cleanup main allocations before returning
                if (comp_inst_internal->core_instance_map) bh_free(comp_inst_internal->core_instance_map);
                for (uint32 j = 0; j < current_runtime_module_idx; ++j) { /* ... (deinstantiate modules) ... */ }
                if (comp_inst_internal->module_instances) bh_free(comp_inst_internal->module_instances);
                if (comp_inst_internal->component_instances) bh_free(comp_inst_internal->component_instances);
                bh_free(comp_inst_internal);
                return NULL;
            }


            LOG_VERBOSE("Attempting to instantiate core module definition %u (instance def %u) using wasm_runtime_instantiate_internal", module_def_idx, i);
            
            new_module_inst = wasm_runtime_instantiate_internal(wasm_module, false, /* is_sub_inst */
                                               parent_exec_env,
                                               wasm_module->default_stack_size, wasm_module->default_heap_size,
                                               NULL, /* host_user_data */
                                               resolved_func_imports, wasm_module->import_function_count,
                                               resolved_global_imports, wasm_module->import_global_count,
                                               resolved_table_imports, wasm_module->import_table_count,
                                               resolved_memory_imports, wasm_module->import_memory_count,
                                               error_buf, error_buf_size);

            if (resolved_func_imports) bh_free(resolved_func_imports);
            if (resolved_global_imports) bh_free(resolved_global_imports);
            if (resolved_table_imports) bh_free(resolved_table_imports);
            if (resolved_memory_imports) bh_free(resolved_memory_imports);

            if (!new_module_inst) {
                set_comp_rt_error_v(error_buf, error_buf_size, "Failed to instantiate core module %u (instance def %u) with internal func: %s",
                                   module_def_idx, i, error_buf);
                // Cleanup already instantiated modules and other allocations
                // Note: core_instance_to_runtime_module_idx_map is now comp_inst_internal->core_instance_map
                if (comp_inst_internal->core_instance_map) bh_free(comp_inst_internal->core_instance_map);
                for (uint32 j = 0; j < current_runtime_module_idx; ++j) {
                    if (comp_inst_internal->module_instances[j]) {
                        wasm_deinstantiate(comp_inst_internal->module_instances[j]);
                    }
                }
                if (comp_inst_internal->module_instances) bh_free(comp_inst_internal->module_instances);
                if (comp_inst_internal->component_instances) bh_free(comp_inst_internal->component_instances);
                bh_free(comp_inst_internal);
                return NULL;
            }
            comp_inst_internal->module_instances[current_runtime_module_idx] = new_module_inst;
            if (core_instance_to_runtime_module_idx_map) { // Should exist if count > 0
                core_instance_to_runtime_module_idx_map[i] = current_runtime_module_idx;
            }
            current_runtime_module_idx++;
            LOG_VERBOSE("Successfully instantiated core module definition %u as runtime module instance %u", module_def_idx, current_runtime_module_idx -1);

        } else if (core_instance_def->kind == CORE_INSTANCE_KIND_INLINE_EXPORT) {
            // These don't create a new runtime WASMModuleInstance in our array.
            // They refer to exports of existing module *definitions* or instantiated modules.
            // Their resolution happens when an import needs them or when a component export uses them.
            // We can fill the map with a special value or handle it during lookup.
            if (core_instance_to_runtime_module_idx_map) {
                 // Mark that this core_instance_def_idx does not map to our runtime_module_instances array directly
                 // Or, if it's an alias to an already instantiated module, map it to that runtime index.
                 // This part needs more thought on how CORE_INSTANCE_KIND_INLINE_EXPORT is used for linking.
                 // For now, let's use a sentinel.
                core_instance_to_runtime_module_idx_map[i] = (uint32)-1; // Sentinel for "not directly instantiated here"
            }
        }
    }


    // Placeholder for actual instantiation logic
    LOG_DEBUG("Core module instantiation loop finished. Starting nested component instantiation.");
    uint32 current_runtime_comp_idx = 0;

    // TODO: This loop also assumes instances can be created in the order they are defined.
    // Dependency analysis might be needed for correct instantiation order of nested components
    // if their arguments depend on each other.
    for (i = 0; i < component->component_instance_count; ++i) {
        WASMComponentCompInstance *comp_instance_def = &component->component_instances[i];

        if (comp_instance_def->kind == COMPONENT_INSTANCE_KIND_INSTANTIATE) {
            uint32 nested_comp_def_idx = comp_instance_def->u.instantiate.component_idx;
            WASMComponent *nested_component_def = component->nested_components[nested_comp_def_idx];
            WASMComponentInstanceInternal *new_nested_comp_inst = NULL;

            // TODO: MAJOR - Resolve arguments for the nested component.
            // Arguments are in comp_instance_def->u.instantiate.args (WASMComponentCompInstanceArg).
            // Each arg provides: name (import name for nested comp), kind, and instance_idx (source in outer comp).
            // 'instance_idx' refers to an index in the *outer* component's:
            //   - component->imports (if item_kind in arg is e.g. COMPONENT_ITEM_KIND_COMPONENT_IMPORT)
            //   - comp_inst_internal->module_instances (via core_instance_map, if item_kind is core like FUNC, MEM, etc.)
            //   - comp_inst_internal->component_instances (for already instantiated nested components)
            // This requires a robust lookup mechanism for runtime values (lifted functions, module instances, etc.)
            // and then packaging these resolved imports to be passed to the recursive call.
            // The signature of wasm_component_instance_instantiate needs to be adapted to accept this.
            // For now, passing NULL for imports to the recursive call, which will only work if the
            // nested component has no imports.
            LOG_VERBOSE("Attempting to instantiate nested component definition %u (instance def %u)", nested_comp_def_idx, i);

            ResolvedComponentImportItem *nested_imports_resolved = NULL;
            uint32 num_nested_imports_to_resolve = comp_instance_def->u.instantiate.arg_count; // Or from nested_component_def->import_count?
                                                                                             // The args are what *this* component provides.
                                                                                             // The nested_component_def->import_count is what it *needs*.
                                                                                             // These should match up if validation passed.
                                                                                             // Let's use nested_component_def->import_count for allocation size.
            if (nested_component_def->import_count > 0) {
                nested_imports_resolved = bh_malloc(nested_component_def->import_count * sizeof(ResolvedComponentImportItem));
                if (!nested_imports_resolved) {
                    set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate memory for nested component's resolved imports.");
                    // Full cleanup needed before returning
                    if (comp_inst_internal->core_instance_map) { bh_free(comp_inst_internal->core_instance_map); comp_inst_internal->core_instance_map = NULL; }
                    for (uint32 j = 0; j < comp_inst_internal->num_module_instances; ++j) { /* ... deinstantiate ... */ }
                    if (comp_inst_internal->module_instances) { bh_free(comp_inst_internal->module_instances); comp_inst_internal->module_instances = NULL; }
                    for (uint32 j = 0; j < current_runtime_comp_idx; ++j) { /* ... deinstantiate ... */ }
                    if (comp_inst_internal->component_instances) { bh_free(comp_inst_internal->component_instances); comp_inst_internal->component_instances = NULL; }
                    bh_free(comp_inst_internal);
                    return NULL;
                }
                memset(nested_imports_resolved, 0, nested_component_def->import_count * sizeof(ResolvedComponentImportItem));
            }
            
            bool nested_import_res_failed = false;
            uint32 nested_resolved_idx = 0; 

            // Iterate over arguments specified by the outer component for the nested component's imports.
            // `comp_instance_def->u.instantiate.args` are these arguments.
            // `nested_component_def->imports` are the actual import declarations of the nested component.
            for (uint32 arg_k = 0; arg_k < comp_instance_def->u.instantiate.arg_count; ++arg_k) {
                WASMComponentCompInstanceArg *arg = &comp_instance_def->u.instantiate.args[arg_k];
                uint32 source_instance_index = arg->instance_idx; // Index in outer component's definition space

                // Find the corresponding import definition in the nested component by matching the argument name.
                // This gives us the expected type (`WASMComponentExternDesc`) for the import.
                WASMComponentImport *nested_import_def = NULL;
                // uint32 target_nested_import_idx = (uint32)-1; /* To find the actual import desc */
                for (uint32 import_idx = 0; import_idx < nested_component_def->import_count; ++import_idx) {
                    if (strcmp(nested_component_def->imports[import_idx].name, arg->name) == 0) {
                        nested_import_def = &nested_component_def->imports[import_idx];
                        // target_nested_import_idx = import_idx;
                        break;
                    }
                }

                if (!nested_import_def) {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': no matching import found in nested component definition '%s'.",
                                       arg->name, nested_component_def->name ? nested_component_def->name : "unnamed_nested_component");
                    nested_import_res_failed = true;
                    break; // from arg_k loop
                }

                // Basic kind compatibility check:
                // `arg->kind.item_kind` is what the outer component's argument list claims the item is (e.g., COMPONENT_ITEM_KIND_FUNC).
                // `nested_import_def->desc.kind` is what the nested component's import expects (e.g., EXTERN_DESC_KIND_FUNC).
                bool basic_kind_compatible = false;
                switch (arg->kind.item_kind) {
                    case COMPONENT_ITEM_KIND_FUNC: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_FUNC); break;
                    case COMPONENT_ITEM_KIND_GLOBAL: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_VALUE); break;
                    case COMPONENT_ITEM_KIND_TABLE: /* TODO: Define EXTERN_DESC_KIND_TABLE for component tables if different from core */
                                                  basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_TABLE_NOT_YET_DEFINED_IN_EXTERN_DESC); break;
                    case COMPONENT_ITEM_KIND_MEMORY:/* TODO: Define EXTERN_DESC_KIND_MEMORY for component memories if different from core */
                                                  basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_MEMORY_NOT_YET_DEFINED_IN_EXTERN_DESC); break;
                    case COMPONENT_ITEM_KIND_MODULE: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_MODULE); break;
                    case COMPONENT_ITEM_KIND_COMPONENT: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_COMPONENT); break;
                    case COMPONENT_ITEM_KIND_INSTANCE: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_INSTANCE); break;
                    case COMPONENT_ITEM_KIND_TYPE: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_TYPE); break;
                    case COMPONENT_ITEM_KIND_VALUE: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_VALUE); break;
                    default: basic_kind_compatible = false; break;
                }
                if (!basic_kind_compatible) {
                     set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': basic kind mismatch. Outer provides resolved kind %u, nested expects import desc kind %u.",
                                           arg->name, arg->kind.item_kind, nested_import_def->desc.kind);
                    nested_import_res_failed = true;
                    break; // from arg_k loop
                }

                LOG_VERBOSE("Resolving arg '%s' for nested component '%s', source_idx %u in outer. Outer provides kind %u, Nested expects desc kind %u.",
                            arg->name, nested_component_def->name ? nested_component_def->name : "unnamed",
                            source_instance_index, arg->kind.item_kind, nested_import_def->desc.kind);

                WASMComponentImport *outer_definition_component_imports = comp_inst_internal->component_def->imports;
                uint32 num_outer_definition_component_imports = comp_inst_internal->component_def->import_count;
                uint32 num_outer_definition_core_instances = comp_inst_internal->component_def->core_instance_count;

                if (source_instance_index < num_outer_definition_component_imports) {
                    // Source is an import of the current (outer) component.
                    WASMComponentImport *outer_import_def_for_source_item = &outer_definition_component_imports[source_instance_index];
                    ResolvedComponentImportItem *resolved_outer_import_item = NULL;

                    for (uint32 k = 0; k < comp_inst_internal->num_resolved_imports; ++k) {
                        if (strcmp(comp_inst_internal->resolved_imports[k].name, outer_import_def_for_source_item->name) == 0) {
                            resolved_outer_import_item = &comp_inst_internal->resolved_imports[k];
                            break;
                        }
                    }

                    if (!resolved_outer_import_item) {
                        set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': required outer component import '%s' (def_idx %u) was not resolved/provided to outer component.",
                                           arg->name, outer_import_def_for_source_item->name, source_instance_index);
                        nested_import_res_failed = true;
                    } else if (resolved_outer_import_item->kind != arg->kind.item_kind) {
                        // This checks if the runtime type of the resolved outer import matches what the outer component's arg list *claims* it is.
                        set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': kind mismatch. Arg from outer def expects kind %u, but host provided %u for outer import '%s'.",
                                           arg->name, arg->kind.item_kind, resolved_outer_import_item->kind, outer_import_def_for_source_item->name);
                        nested_import_res_failed = true;
                    } else {
                        // TODO: Detailed type check: resolved_outer_import_item->desc (if we add it) vs nested_import_def->desc
                        LOG_TODO("Detailed type check for nested import '%s' from outer import '%s'.", arg->name, outer_import_def_for_source_item->name);
                        nested_imports_resolved[nested_resolved_idx].name = bh_strdup(arg->name);
                        if (!nested_imports_resolved[nested_resolved_idx].name) { nested_import_res_failed = true; }
                        else {
                            nested_imports_resolved[nested_resolved_idx].kind = resolved_outer_import_item->kind;
                            nested_imports_resolved[nested_resolved_idx].item = resolved_outer_import_item->item; // Shallow copy
                            nested_resolved_idx++;
                        }
                    }
                } else if (source_instance_index < num_outer_definition_component_imports + num_outer_definition_core_instances) {
                    uint32 core_instance_def_idx = source_instance_index - num_outer_definition_component_imports;
                    WASMComponentCoreInstance *src_core_inst_def = &comp_inst_internal->component_def->core_instances[core_instance_def_idx];

                    if (src_core_inst_def->kind != CORE_INSTANCE_KIND_INSTANTIATE) {
                         set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': source core instance def %u is an inline export, not directly usable as argument source here.", arg->name, core_instance_def_idx);
                         nested_import_res_failed = true;
                         break; 
                    }
                    uint32 src_runtime_mod_arr_idx = comp_inst_internal->core_instance_map[core_instance_def_idx];
                    if (src_runtime_mod_arr_idx == (uint32)-1) { // Should not happen for INSTANTIATE kind if map is correct
                        set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': source core instance def %u mapped to invalid runtime module index.", arg->name, core_instance_def_idx);
                        nested_import_res_failed = true;
                    } else {
                        WASMModuleInstance *src_mod_inst = comp_inst_internal->module_instances[src_runtime_mod_arr_idx];
                        if (!src_mod_inst) { // Should not happen
                            set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': source core module instance (def_idx %u, runtime_idx %u) is NULL.",
                                               arg->name, core_instance_def_idx, src_runtime_mod_arr_idx);
                            nested_import_res_failed = true;
                        } else {
                            const char *export_name_from_source_core_module = arg->name; // Assume arg name is export name

                            nested_imports_resolved[nested_resolved_idx].name = bh_strdup(arg->name);
                            if (!nested_imports_resolved[nested_resolved_idx].name) { nested_import_res_failed = true; }
                            else {
                                // arg->kind.item_kind is what the outer component's arg list claims this item is (e.g. COMPONENT_ITEM_KIND_FUNC)
                                // This was already checked against nested_import_def->desc.kind
                                nested_imports_resolved[nested_resolved_idx].kind = arg->kind.item_kind;

                                switch (arg->kind.item_kind) {
                                    case COMPONENT_ITEM_KIND_FUNC: {
                                        WASMFunctionInstance *func = find_exported_function_instance(src_mod_inst, export_name_from_source_core_module, error_buf, error_buf_size);
                                        if (!func) { nested_import_res_failed = true; break; }
                                        // TODO: Detailed type check: func->type vs nested_import_def->desc.u.func_type_idx (needs outer component's type context for the index)
                                        LOG_TODO("Detailed type check for func import '%s' from core export.", arg->name);
                                        nested_imports_resolved[nested_resolved_idx].item.function = func;
                                        break;
                                    }
                                    case COMPONENT_ITEM_KIND_GLOBAL: {
                                        // Mutability for find_exported_global_instance should ideally come from nested_import_def.
                                        // nested_import_def->desc.kind == EXTERN_DESC_KIND_VALUE.
                                        // nested_import_def->desc.u.value_type is a WASMComponentValType*.
                                        // Core Wasm global mutability is not directly part of WASMComponentValType.
                                        // This implies a potential gap or that mutability is only checked if this global is passed to another core module.
                                        // For now, we pass a placeholder. The original code used `arg->kind.u.global.is_mutable`
                                        // but `arg->kind` here is `ResolvedComponentItemKind` which has no `u.global`.
                                        // The `WASMComponentCompInstanceArg.kind` is `WASMComponentItemKind` which also doesn't have `u.global`.
                                        // This was a bug in the original template for this section.
                                        // The mutability must be part of the nested_import_def's type description if it's a core global.
                                        // This is complex. For now, assume the caller of find_exported_global_instance will handle mutability check if it's re-exported to another core module.
                                        // If the component itself uses the global, the component type system for values should suffice.
                                        // Let's pass 'false' as a placeholder, acknowledging this is incomplete.
                                        LOG_TODO("Mutability for global import '%s' from core export needs robust handling. Passing false placeholder.", arg->name);
                                        WASMGlobalInstance *global = find_exported_global_instance(src_mod_inst, export_name_from_source_core_module,
                                                                                                 false, /* placeholder for expected_mutability */
                                                                                                 error_buf, error_buf_size);
                                        if (!global) { nested_import_res_failed = true; break; }
                                        // TODO: Detailed type check: global->type/mutability vs nested_import_def->desc.u.value_type
                                        LOG_TODO("Detailed type check for global import '%s' from core export.", arg->name);
                                        nested_imports_resolved[nested_resolved_idx].item.global = global;
                                        break;
                                    }
                                    case COMPONENT_ITEM_KIND_TABLE: {
                                        WASMTableInstance *tbl = find_exported_table_instance(src_mod_inst, export_name_from_source_core_module, error_buf, error_buf_size);
                                        if (!tbl) { nested_import_res_failed = true; break; }
                                        LOG_TODO("Detailed type check for table import '%s' from core export.", arg->name);
                                        nested_imports_resolved[nested_resolved_idx].item.table = tbl;
                                        break;
                                    }
                                    case COMPONENT_ITEM_KIND_MEMORY: {
                                        WASMMemoryInstance *mem = find_exported_memory_instance(src_mod_inst, export_name_from_source_core_module, error_buf, error_buf_size);
                                        if (!mem) { nested_import_res_failed = true; break; }
                                        LOG_TODO("Detailed type check for memory import '%s' from core export.", arg->name);
                                        nested_imports_resolved[nested_resolved_idx].item.memory = mem;
                                        break;
                                    }
                                    // Not supporting direct import of MODULE, COMPONENT, INSTANCE, TYPE, VALUE from core module exports yet.
                                    // These are component-level concepts.
                                    default:
                                        set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': unhandled item kind %u (from arg provider) for core module export source.", arg->name, arg->kind.item_kind);
                                        nested_import_res_failed = true;
                                        break;
                                }
                                if (nested_import_res_failed && nested_imports_resolved[nested_resolved_idx].name) {
                                    bh_free(nested_imports_resolved[nested_resolved_idx].name);
                                    nested_imports_resolved[nested_resolved_idx].name = NULL;
                                } else if (!nested_import_res_failed) {
                                   nested_resolved_idx++;
                                }
                            }
                        }
                    }
                }
                // TODO: Handle source being another nested component instance (comp_inst_internal->component_instances)
                // else if (source_instance_index < num_outer_definition_component_imports + num_outer_definition_core_instances + num_outer_definition_nested_components) { ... }
                // TODO: Handle source being an alias (requires resolving the alias first from comp_inst_internal->component_def->aliases)
                else {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': source instance index %u out of currently supported range or unhandled source type.", arg->name, source_instance_index);
                    nested_import_res_failed = true;
                }
                if (nested_import_res_failed) break; // Break from arg_k loop
            } // End loop over args

            if (nested_import_res_failed || nested_resolved_idx != nested_component_def->import_count) {
                if (!nested_import_res_failed) { // Only set error if not already set
                    set_comp_rt_error_v(error_buf, error_buf_size, "Failed to resolve all imports for nested component %s (resolved %u, need %u).",
                                   nested_component_def->name, nested_resolved_idx, nested_component_def->import_count);
                }
                // Free any strduped names in nested_imports_resolved
                for(uint32 k=0; k < nested_resolved_idx; ++k) if(nested_imports_resolved[k].name) bh_free(nested_imports_resolved[k].name);
                if (nested_imports_resolved) bh_free(nested_imports_resolved);
                // Full cleanup is complex here, as a partial failure might need to unwind.
                // For now, the main instantiation failure path will handle broader cleanup.
                // This indicates an error that should bubble up.
                // Let's ensure the main error path is triggered if new_nested_comp_inst is NULL after this.
                new_nested_comp_inst = NULL; // Ensure this to trigger cleanup below
            }

            if (!nested_import_res_failed && nested_resolved_idx == nested_component_def->import_count) {
                 LOG_VERBOSE("All %u imports for nested component %s resolved. Attempting instantiation.", nested_resolved_idx, nested_component_def->name);
                 new_nested_comp_inst = wasm_component_instance_instantiate(
                    nested_component_def,
                    parent_exec_env, 
                    nested_imports_resolved, nested_resolved_idx, /* Pass resolved imports */
                    error_buf, error_buf_size);
            } else if (!nested_import_res_failed && nested_resolved_idx != nested_component_def->import_count) {
                // This case should have been caught above, but as a safeguard:
                set_comp_rt_error_v(error_buf, error_buf_size, "Import count mismatch for nested component %s (resolved %u, need %u) after loop.",
                                   nested_component_def->name, nested_resolved_idx, nested_component_def->import_count);
                new_nested_comp_inst = NULL; // Ensure failure path
            }
            // If nested_import_res_failed is true, new_nested_comp_inst remains NULL or is set to NULL.


            if (nested_imports_resolved) { // Free regardless of success/failure of instantiate call
                for(uint32 k=0; k < nested_resolved_idx; ++k) if(nested_imports_resolved[k].name) bh_free(nested_imports_resolved[k].name);
                bh_free(nested_imports_resolved);
            }
            
            if (!new_nested_comp_inst) {
                // If error_buf is empty, it means the recursive call didn't set it, so set a generic one.
                if (error_buf[0] == '\0') { 
                    set_comp_rt_error_v(error_buf, error_buf_size, "Failed to instantiate nested component %u (instance def %u).",
                                   nested_comp_def_idx, i);
                }
                // Cleanup this component instance's partially instantiated state
                // Note: current_runtime_module_idx is from the previous loop for core modules.
                if (comp_inst_internal->core_instance_map) { bh_free(comp_inst_internal->core_instance_map); comp_inst_internal->core_instance_map = NULL; }
                for (uint32 j = 0; j < comp_inst_internal->num_module_instances; ++j) { // Use num_module_instances for full cleanup
                    if (comp_inst_internal->module_instances[j]) wasm_deinstantiate(comp_inst_internal->module_instances[j]);
                }
                if (comp_inst_internal->module_instances) { bh_free(comp_inst_internal->module_instances); comp_inst_internal->module_instances = NULL; }
                for (uint32 j = 0; j < current_runtime_comp_idx; ++j) { // current_runtime_comp_idx is for this loop
                     if (comp_inst_internal->component_instances[j]) wasm_component_instance_deinstantiate(comp_inst_internal->component_instances[j]);
                }
                if (comp_inst_internal->component_instances) { bh_free(comp_inst_internal->component_instances); comp_inst_internal->component_instances = NULL; }
                bh_free(comp_inst_internal);
                return NULL;
            }
            comp_inst_internal->component_instances[current_runtime_comp_idx++] = new_nested_comp_inst;
            LOG_VERBOSE("Successfully instantiated nested component definition %u as runtime component instance %u", nested_comp_def_idx, current_runtime_comp_idx -1);

        } else if (comp_instance_def->kind == COMPONENT_INSTANCE_KIND_FROM_EXPORT) {
            // This represents an import of a component instance into the current component's scope.
            // The actual ComponentInstanceInternal* should come from the 'import_object' or equivalent
            // passed to this function. This resolved import then needs to be stored, perhaps in a separate
            // list or by using the component_instances array with a flag indicating it's not owned.
            // For now, this is a placeholder.
            LOG_TODO("Component import resolution (instance def %u, kind FROM_EXPORT) not implemented.", i);
        }
    }
    LOG_DEBUG("Nested component instantiation loop finished.");

    // Populate Component Exports
    if (!wasm_component_instance_populate_exports(comp_inst_internal, error_buf, error_buf_size)) {
        // Error already set by wasm_component_instance_populate_exports
        // Full cleanup of comp_inst_internal is needed
        if (comp_inst_internal->core_instance_map) { bh_free(comp_inst_internal->core_instance_map); }
        for (uint32 j = 0; j < comp_inst_internal->num_module_instances; ++j) {
            if (comp_inst_internal->module_instances[j]) wasm_deinstantiate(comp_inst_internal->module_instances[j]);
        }
        if (comp_inst_internal->module_instances) { bh_free(comp_inst_internal->module_instances); }
        for (uint32 j = 0; j < comp_inst_internal->num_component_instances; ++j) { // num_component_instances refers to successfully instantiated ones
            if (comp_inst_internal->component_instances[j]) wasm_component_instance_deinstantiate(comp_inst_internal->component_instances[j]);
        }
        if (comp_inst_internal->component_instances) { bh_free(comp_inst_internal->component_instances); }
        // Note: resolved_exports might be partially allocated, populate_exports should clean up its own partial work on error.
        // If populate_exports itself failed to allocate resolved_exports array, it's fine.
        // If it allocated the array then failed, it should free the array.
        // Names within resolved_exports are handled by wasm_component_instance_deinstantiate path.
        bh_free(comp_inst_internal);
        return NULL;
    }

    // Execute Start Function if defined
    if (comp_inst_internal->component_def->start_count > 0) {
        if (comp_inst_internal->component_def->start_count > 1) {
            LOG_WARNING("Multiple start functions defined (%u), only the first one will be executed.", comp_inst_internal->component_def->start_count);
        }
        WASMComponentStart *start_def = &comp_inst_internal->component_def->starts[0];
        if (!execute_component_start_function(comp_inst_internal, start_def, error_buf, error_buf_size)) {
            // Error already set by execute_component_start_function
            // Full cleanup of comp_inst_internal is needed (similar to other failure paths)
            // Re-using the deinstantiate function for cleanup.
            wasm_component_instance_deinstantiate(comp_inst_internal); 
            return NULL;
        }
    }


    // Temporary: return success if allocation succeeded.
    // Real success depends on all instantiation steps including import resolution and linking.
    return comp_inst_internal; 
}

void
wasm_component_instance_deinstantiate(WASMComponentInstanceInternal *comp_inst)
{
    if (!comp_inst) {
        return;
    }

    LOG_DEBUG("Deinstantiating component instance. Module/component deinstantiation logic to be implemented.");

    uint32 i;

    // Deinstantiate core module instances
    if (comp_inst->module_instances) {
        for (i = 0; i < comp_inst->num_module_instances; ++i) {
           if (comp_inst->module_instances[i]) {
               wasm_deinstantiate(comp_inst->module_instances[i]);
           }
        }
        bh_free(comp_inst->module_instances);
    }

    // Deinstantiate nested component instances (recursively)
    if (comp_inst->component_instances) {
        for (i = 0; i < comp_inst->num_component_instances; ++i) {
           if (comp_inst->component_instances[i]) {
               wasm_component_instance_deinstantiate(comp_inst->component_instances[i]);
           }
        }
        bh_free(comp_inst->component_instances);
    }
    
    // TODO: Free resolved exports and imports if dynamically allocated.
    // Note: comp_inst->resolved_imports is not freed here as its lifetime is managed
    // by the caller of wasm_component_instance_instantiate (it's a shallow copy for now).

    // Free resolved exports
    if (comp_inst->resolved_exports) {
        for (i = 0; i < comp_inst->num_resolved_exports; ++i) {
            if (comp_inst->resolved_exports[i].name) {
                bh_free(comp_inst->resolved_exports[i].name);
            }
            // TODO: Free function_thunk_context or other complex items if allocated by populate_exports
            // For now, item union contains pointers to things owned by other parts of the instance or component definition,
            // except for function_thunk_context which might be allocated.
            if (comp_inst->resolved_exports[i].kind == COMPONENT_EXPORT_KIND_FUNC) {
                // Assuming function_thunk_context might need freeing (e.g. if it's LiftedFuncThunk*)
                // This depends on the actual implementation of create_lifted_function_thunk.
                // For now, let's assume it needs to be freed if non-NULL.
                if (comp_inst->resolved_exports[i].item.function_thunk_context) {
                    LOG_TODO("Freeing of function_thunk_context in resolved_exports needs specific logic.");
                    // bh_free(comp_inst->resolved_exports[i].item.function_thunk_context); // Example
                }
            }
        }
        bh_free(comp_inst->resolved_exports);
    }
    
    if (comp_inst->core_instance_map) {
        bh_free(comp_inst->core_instance_map);
    }

    bh_free(comp_inst);
}

// TODO: Implement this function
static bool
instance_type_compatible(WASMComponentInstanceType *expected_inst_type,
                         WASMComponentInstanceInternal *actual_inst,
                         WASMComponent *outer_component_def_context, /* For resolving type indexes in expected_inst_type */
                         char *error_buf, uint32 error_buf_size)
{
    uint32 i;

    if (!expected_inst_type) {
        set_comp_rt_error(error_buf, error_buf_size, "Expected instance type is NULL.");
        return false;
    }
    if (!actual_inst && expected_inst_type->decl_count > 0) {
        set_comp_rt_error(error_buf, error_buf_size, "Actual instance is NULL but expected instance type is not empty.");
        return false;
    }
    if (!actual_inst && expected_inst_type->decl_count == 0) {
        return true; // Both are effectively empty/null
    }
    if (!actual_inst) { // Should have been caught by previous case if decl_count > 0
        set_comp_rt_error(error_buf, error_buf_size, "Actual instance is NULL (unexpected).");
        return false;
    }


    for (i = 0; i < expected_inst_type->decl_count; ++i) {
        WASMComponentInstanceTypeDecl *decl = &expected_inst_type->decls[i];

        if (decl->kind == INSTANCE_TYPE_DECL_KIND_EXPORT) {
            WASMComponentTypeExportDecl *expected_export_decl = &decl->u.export_decl;
            ResolvedComponentExportItem *actual_resolved_export = NULL;
            uint32 j;

            // Find export by name in actual_inst
            for (j = 0; j < actual_inst->num_resolved_exports; ++j) {
                if (strcmp(actual_inst->resolved_exports[j].name, expected_export_decl->name) == 0) {
                    actual_resolved_export = &actual_inst->resolved_exports[j];
                    break;
                }
            }

            if (!actual_resolved_export) {
                set_comp_rt_error_v(error_buf, error_buf_size,
                                   "Expected export '%s' not found in actual instance.",
                                   expected_export_decl->name);
                return false;
            }

            // Check kind compatibility
            // actual_resolved_export->kind is ResolvedComponentExportItemKind
            // expected_export_decl->desc.kind is WASMComponentExternDescKind
            bool kind_compatible = false;
            switch (expected_export_decl->desc.kind) {
                case EXTERN_DESC_KIND_FUNC:
                    kind_compatible = (actual_resolved_export->kind == COMPONENT_EXPORT_KIND_FUNC);
                    break;
                case EXTERN_DESC_KIND_INSTANCE:
                    kind_compatible = (actual_resolved_export->kind == COMPONENT_EXPORT_KIND_INSTANCE);
                    break;
                case EXTERN_DESC_KIND_COMPONENT:
                    kind_compatible = (actual_resolved_export->kind == COMPONENT_EXPORT_KIND_COMPONENT);
                    break;
                case EXTERN_DESC_KIND_MODULE: // Core module
                    kind_compatible = (actual_resolved_export->kind == COMPONENT_EXPORT_KIND_MODULE);
                    break;
                case EXTERN_DESC_KIND_VALUE: // Exported global/value
                    kind_compatible = (actual_resolved_export->kind == COMPONENT_EXPORT_KIND_VALUE);
                    break;
                case EXTERN_DESC_KIND_TYPE:
                    kind_compatible = (actual_resolved_export->kind == COMPONENT_EXPORT_KIND_TYPE);
                    break;
                default:
                    kind_compatible = false;
            }

            if (!kind_compatible) {
                set_comp_rt_error_v(error_buf, error_buf_size,
                                   "Export '%s': kind mismatch. Expected extern_desc kind %u, actual export kind %u.",
                                   expected_export_decl->name, expected_export_decl->desc.kind, actual_resolved_export->kind);
                return false;
            }

            // Recursive type check based on kind
            switch (expected_export_decl->desc.kind) {
                case EXTERN_DESC_KIND_FUNC:
                {
                    if (expected_export_decl->desc.u.func_type_idx >= outer_component_def_context->type_definition_count ||
                        outer_component_def_context->type_definitions[expected_export_decl->desc.u.func_type_idx].kind != DEF_TYPE_KIND_FUNC) {
                        set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': expected func type index %u invalid.", expected_export_decl->name, expected_export_decl->desc.u.func_type_idx);
                        return false;
                    }
                    WASMComponentFuncType *expected_func_type = &outer_component_def_context->type_definitions[expected_export_decl->desc.u.func_type_idx].u.func_type;
                    LiftedFuncThunkContext *thunk_ctx = (LiftedFuncThunkContext*)actual_resolved_export->item.function_thunk_context;
                    if (!thunk_ctx || !thunk_ctx->component_func_type) {
                         set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': actual function thunk or its type info is missing.", expected_export_decl->name);
                         return false;
                    }
                    WASMComponentFuncType *actual_func_type = thunk_ctx->component_func_type;
                    // Actual defining component for the thunk's type is the actual_inst's component_def
                    if (!component_func_type_compatible(expected_func_type, actual_func_type,
                                                        outer_component_def_context, actual_inst->component_def,
                                                        error_buf, error_buf_size)) {
                        // error_buf should be set by component_func_type_compatible
                        return false;
                    }
                    break;
                }
                case EXTERN_DESC_KIND_INSTANCE:
                {
                    if (expected_export_decl->desc.u.instance_type_idx >= outer_component_def_context->type_definition_count ||
                        outer_component_def_context->type_definitions[expected_export_decl->desc.u.instance_type_idx].kind != DEF_TYPE_KIND_INSTANCE) {
                        set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': expected instance type index %u invalid.", expected_export_decl->name, expected_export_decl->desc.u.instance_type_idx);
                        return false;
                    }
                    WASMComponentInstanceType *expected_sub_inst_type = outer_component_def_context->type_definitions[expected_export_decl->desc.u.instance_type_idx].u.inst_type;
                    WASMComponentInstanceInternal *actual_sub_inst = actual_resolved_export->item.component_instance;
                    if (!instance_type_compatible(expected_sub_inst_type, actual_sub_inst,
                                                  outer_component_def_context, /* Pass outer context for nested expected types */
                                                  error_buf, error_buf_size)) {
                        return false;
                    }
                    break;
                }
                case EXTERN_DESC_KIND_COMPONENT:
                {
                     if (expected_export_decl->desc.u.component_type_idx >= outer_component_def_context->type_definition_count ||
                        outer_component_def_context->type_definitions[expected_export_decl->desc.u.component_type_idx].kind != DEF_TYPE_KIND_COMPONENT) {
                        set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': expected component type index %u invalid.", expected_export_decl->name, expected_export_decl->desc.u.component_type_idx);
                        return false;
                    }
                    WASMComponentComponentType *expected_sub_comp_type = outer_component_def_context->type_definitions[expected_export_decl->desc.u.component_type_idx].u.comp_type;
                    WASMComponent *actual_sub_comp_def = actual_resolved_export->item.component_definition;
                     if (!component_type_compatible(expected_sub_comp_type, actual_sub_comp_def,
                                                   outer_component_def_context, /* Pass outer context for nested expected types */
                                                   error_buf, error_buf_size)) {
                        return false;
                    }
                    break;
                }
                case EXTERN_DESC_KIND_MODULE: // Core module
                {
                    // Ensure core_type_defs is used for core_module_type_idx
                    if (expected_export_decl->desc.u.core_module_type_idx >= outer_component_def_context->core_type_count ||
                        outer_component_def_context->core_types[expected_export_decl->desc.u.core_module_type_idx].kind != CORE_TYPE_KIND_MODULE_OBSOLETE) { // Assuming CORE_TYPE_KIND_MODULE_OBSOLETE or similar for module types in core_types
                        set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': expected core module type index %u invalid or not a module type.", expected_export_decl->name, expected_export_decl->desc.u.core_module_type_idx);
                        // LOG_TODO: Fix kind check once CORE_TYPE_KIND_MODULE is finalized for core_types section
                        // return false; // Temporarily allow to pass if CORE_TYPE_KIND_MODULE_OBSOLETE is not used
                    }
                    // WASMComponentCoreModuleType *expected_core_mod_type = &outer_component_def_context->core_types[expected_export_decl->desc.u.core_module_type_idx].u.core_type_def.module_type; // If structure was different
                    // This needs to align with how core module types are stored and retrieved.
                    // For now, assuming it's not DEF_TYPE_KIND_CORE_MODULE from type_definitions, but a core_type.
                    LOG_TODO("Export '%s': core_module_type_compatible needs to be called. Type resolution path for expected core module type needs confirmation.", expected_export_decl->name);
                    // WASMModuleInstance *actual_core_mod_inst = actual_resolved_export->item.module_instance;
                    // if (!core_module_type_compatible(expected_core_mod_type, actual_core_mod_inst, outer_component_def_context, error_buf, error_buf_size)) {
                    //    return false;
                    // }
                    break;
                }
                case EXTERN_DESC_KIND_VALUE: // Exported global/value
                {
                    // expected_export_decl->desc.u.value_type is WASMComponentValType*
                    WASMComponentValType *expected_val_type = expected_export_decl->desc.u.value_type;
                    WASMGlobalInstance *actual_global = actual_resolved_export->item.global;
                    if (!core_global_type_compatible_with_component_val_type(expected_val_type, actual_global,
                                                                            outer_component_def_context,
                                                                            error_buf, error_buf_size)) {
                        return false;
                    }
                    break;
                }
                case EXTERN_DESC_KIND_TYPE:
                {
                    LOG_TODO("Type compatibility for EXTERN_DESC_KIND_TYPE (exported types with bounds) in instance_type_compatible not fully implemented.");
                    // WASMComponentTypeBound *expected_type_bound = &expected_export_decl->desc.u.type_bound;
                    // WASMComponentDefinedType *actual_type_def = actual_resolved_export->item.type_definition;
                    // Implement type bound checking logic here.
                    break;
                }
                default:
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Unhandled extern_desc kind %u for compatibility check.",
                                       expected_export_decl->name, expected_export_decl->desc.kind);
                    return false;
            }
        }
        // TODO: Handle other decl kinds if necessary (e.g. aliases, types defined within instance type)
        // For now, only exports are checked for subtype compatibility as per typical instance subtyping.
    }

    return true;
}

// Helper for component_type_compatible
static bool
extern_desc_compatible(WASMComponentExternDesc *expected_desc,
                       WASMComponentExternDesc *actual_desc,
                       WASMComponent *context_for_expected,
                       WASMComponent *context_for_actual,
                       char *error_buf, uint32 error_buf_size)
{
    if (expected_desc->kind != actual_desc->kind) {
        set_comp_rt_error_v(error_buf, error_buf_size,
                           "Extern description kind mismatch. Expected %d, actual %d.",
                           expected_desc->kind, actual_desc->kind);
        return false;
    }

    switch (expected_desc->kind) {
        case EXTERN_DESC_KIND_FUNC:
        {
            if (expected_desc->u.func_type_idx >= context_for_expected->type_definition_count
                || context_for_expected->type_definitions[expected_desc->u.func_type_idx].kind != DEF_TYPE_KIND_FUNC) {
                set_comp_rt_error(error_buf, error_buf_size, "Invalid expected function type index.");
                return false;
            }
            WASMComponentFuncType *expected_ft = &context_for_expected->type_definitions[expected_desc->u.func_type_idx].u.func_type;

            if (actual_desc->u.func_type_idx >= context_for_actual->type_definition_count
                || context_for_actual->type_definitions[actual_desc->u.func_type_idx].kind != DEF_TYPE_KIND_FUNC) {
                set_comp_rt_error(error_buf, error_buf_size, "Invalid actual function type index.");
                return false;
            }
            WASMComponentFuncType *actual_ft = &context_for_actual->type_definitions[actual_desc->u.func_type_idx].u.func_type;
            return component_func_type_compatible(expected_ft, actual_ft, context_for_expected, context_for_actual, error_buf, error_buf_size);
        }
        case EXTERN_DESC_KIND_INSTANCE:
        {
            if (expected_desc->u.instance_type_idx >= context_for_expected->type_definition_count
                || context_for_expected->type_definitions[expected_desc->u.instance_type_idx].kind != DEF_TYPE_KIND_INSTANCE) {
                set_comp_rt_error(error_buf, error_buf_size, "Invalid expected instance type index.");
                return false;
            }
            WASMComponentInstanceType *expected_it = context_for_expected->type_definitions[expected_desc->u.instance_type_idx].u.inst_type;

            if (actual_desc->u.instance_type_idx >= context_for_actual->type_definition_count
                || context_for_actual->type_definitions[actual_desc->u.instance_type_idx].kind != DEF_TYPE_KIND_INSTANCE) {
                set_comp_rt_error(error_buf, error_buf_size, "Invalid actual instance type index.");
                return false;
            }
            // instance_type_compatible expects WASMComponentInstanceInternal for actual, not WASMComponentInstanceType.
            // This helper is for comparing type definitions, not runtime instances.
            // This implies a different version of instance_type_compatible for definition-time checks,
            // or this path is invalid for definition-time extern_desc comparison.
            LOG_TODO("EXTERN_DESC_KIND_INSTANCE compatibility check needs instance_type_definition_compatible().");
            // For now, assume instance_type_compatible can somehow work or this is a TODO.
            // WASMComponentInstanceType *actual_it = context_for_actual->type_definitions[actual_desc->u.instance_type_idx].u.inst_type;
            // return instance_type_compatible(expected_it, actual_it /* This is wrong */, context_for_expected, error_buf, error_buf_size);
            return true; // Placeholder
        }
        case EXTERN_DESC_KIND_COMPONENT:
        {
            if (expected_desc->u.component_type_idx >= context_for_expected->type_definition_count
                || context_for_expected->type_definitions[expected_desc->u.component_type_idx].kind != DEF_TYPE_KIND_COMPONENT) {
                set_comp_rt_error(error_buf, error_buf_size, "Invalid expected component type index.");
                return false;
            }
            WASMComponentComponentType *expected_ct = context_for_expected->type_definitions[expected_desc->u.component_type_idx].u.comp_type;

            if (actual_desc->u.component_type_idx >= context_for_actual->type_definition_count
                || context_for_actual->type_definitions[actual_desc->u.component_type_idx].kind != DEF_TYPE_KIND_COMPONENT) {
                set_comp_rt_error(error_buf, error_buf_size, "Invalid actual component type index.");
                return false;
            }
            // component_type_compatible expects WASMComponent for actual, not WASMComponentComponentType.
            // This implies a different version of component_type_compatible for definition-time checks.
            LOG_TODO("EXTERN_DESC_KIND_COMPONENT compatibility check needs component_type_definition_compatible().");
            // WASMComponentComponentType *actual_ct = context_for_actual->type_definitions[actual_desc->u.component_type_idx].u.comp_type;
            // return component_type_compatible(expected_ct, actual_ct /* This is wrong */, context_for_expected, error_buf, error_buf_size);
            return true; // Placeholder
        }
        case EXTERN_DESC_KIND_MODULE: // Core Module
        {
            // Expected type from context_for_expected->core_types
            if (expected_desc->u.core_module_type_idx >= context_for_expected->core_type_count
                || context_for_expected->core_types[expected_desc->u.core_module_type_idx].kind != CORE_TYPE_KIND_MODULE_OBSOLETE /* Placeholder kind */) {
                set_comp_rt_error(error_buf, error_buf_size, "Invalid expected core module type index or kind.");
                return false;
            }
            // WASMComponentCoreModuleType *expected_cmt = &context_for_expected->core_types[expected_desc->u.core_module_type_idx].u.core_type_def.module_type; // If using new structure

            // Actual type from context_for_actual->core_types
            if (actual_desc->u.core_module_type_idx >= context_for_actual->core_type_count
                || context_for_actual->core_types[actual_desc->u.core_module_type_idx].kind != CORE_TYPE_KIND_MODULE_OBSOLETE /* Placeholder kind */) {
                set_comp_rt_error(error_buf, error_buf_size, "Invalid actual core module type index or kind.");
                return false;
            }
            // WASMComponentCoreModuleType *actual_cmt = &context_for_actual->core_types[actual_desc->u.core_module_type_idx].u.core_type_def.module_type;
            LOG_TODO("EXTERN_DESC_KIND_MODULE compatibility using core_module_type_compatible (needs review of type storage).");
            // return core_module_type_compatible(expected_cmt, actual_cmt, error_buf, error_buf_size);
            return true; // Placeholder
        }
        case EXTERN_DESC_KIND_VALUE:
            // If WASMComponentExternDesc.u.value_type is WASMComponentValType (direct struct)
            return component_val_type_compatible(&expected_desc->u.value_type, &actual_desc->u.value_type,
                                               context_for_expected, context_for_actual, error_buf, error_buf_size);
        }
        case EXTERN_DESC_KIND_TYPE:
            LOG_TODO("Type bound compatibility for EXTERN_DESC_KIND_TYPE not fully implemented in extern_desc_compatible.");
            if (expected_desc->u.type_bound.kind == TYPE_BOUND_KIND_EQ && actual_desc->u.type_bound.kind == TYPE_BOUND_KIND_EQ) {
                // This is a very simplistic check. Type equality usually means structural equivalence,
                // or if they point to the same type definition *in the same component definition context*.
                // Here we only have indices, so they must be in the same context for direct index comparison.
                // If contexts are different, this is insufficient.
                if (context_for_expected == context_for_actual) {
                    if (expected_desc->u.type_bound.type_idx == actual_desc->u.type_bound.type_idx) {
                        return true;
                    }
                }
                // Fallback to structural check or error if contexts differ / more complex bound.
                set_comp_rt_error(error_buf, error_buf_size, "Type bound compatibility for EQ across different contexts or non-EQ bounds not yet fully supported.");
                return false; // Or true if we want to be lenient for now.
            }
            return false; // Different bound kinds or unhandled
        default:
            set_comp_rt_error_v(error_buf, error_buf_size, "Unknown extern description kind %d for compatibility check.", expected_desc->kind);
            return false;
    }
    // Should be unreachable if all cases return
    return false;
}

static bool
component_type_compatible(WASMComponentComponentType *expected_comp_type,
                          WASMComponent *actual_comp_def,
                          WASMComponent *defining_context_for_expected_type, /* Component that defines expected_comp_type */
                          char* error_buf, uint32 error_buf_size)
{
    uint32 i, j;

    if (!expected_comp_type) {
        set_comp_rt_error(error_buf, error_buf_size, "Expected component type is NULL.");
        return false;
    }
    if (!actual_comp_def) {
        set_comp_rt_error(error_buf, error_buf_size, "Actual component definition is NULL.");
        return false;
    }

    // Check imports
    for (i = 0; i < expected_comp_type->decl_count; ++i) {
        if (expected_comp_type->decls[i].kind == COMPONENT_TYPE_DECL_KIND_IMPORT) {
            WASMComponentTypeImportDecl *expected_import_decl = &expected_comp_type->decls[i].u.import_decl;
            WASMComponentImport *actual_import_def = NULL;

            for (j = 0; j < actual_comp_def->import_count; ++j) {
                if (strcmp(actual_comp_def->imports[j].name, expected_import_decl->name) == 0) {
                    actual_import_def = &actual_comp_def->imports[j];
                    break;
                }
            }

            if (!actual_import_def) {
                set_comp_rt_error_v(error_buf, error_buf_size,
                                   "Expected import '%s' not found in actual component definition.",
                                   expected_import_decl->name);
                return false;
            }

            // Compare descriptions
            if (!extern_desc_compatible(&expected_import_decl->desc, &actual_import_def->desc,
                                        defining_context_for_expected_type, actual_comp_def, /* actual_comp_def is context for its own imports */
                                        error_buf, error_buf_size)) {
                // error_buf should be set by extern_desc_compatible
                return false;
            }
        }
    }

    // Check exports
    for (i = 0; i < expected_comp_type->decl_count; ++i) {
        if (expected_comp_type->decls[i].kind == COMPONENT_TYPE_DECL_KIND_EXPORT) {
            WASMComponentTypeExportDecl *expected_export_decl = &expected_comp_type->decls[i].u.export_decl;
            WASMComponentExport *actual_export_def = NULL;

            for (j = 0; j < actual_comp_def->export_count; ++j) {
                if (strcmp(actual_comp_def->exports[j].name, expected_export_decl->name) == 0) {
                    actual_export_def = &actual_comp_def->exports[j];
                    break;
                }
            }

            if (!actual_export_def) {
                set_comp_rt_error_v(error_buf, error_buf_size,
                                   "Expected export '%s' not found in actual component definition.",
                                   expected_export_decl->name);
                return false;
            }

            // Basic kind check (e.g. EXPORT_KIND_FUNC vs EXTERN_DESC_KIND_FUNC)
            bool kind_match = false;
            switch(expected_export_decl->desc.kind) {
                case EXTERN_DESC_KIND_FUNC: kind_match = (actual_export_def->kind == EXPORT_KIND_FUNC); break;
                case EXTERN_DESC_KIND_VALUE: kind_match = (actual_export_def->kind == EXPORT_KIND_VALUE); break;
                case EXTERN_DESC_KIND_TYPE: kind_match = (actual_export_def->kind == EXPORT_KIND_TYPE); break;
                case EXTERN_DESC_KIND_COMPONENT: kind_match = (actual_export_def->kind == EXPORT_KIND_COMPONENT); break;
                case EXTERN_DESC_KIND_INSTANCE: kind_match = (actual_export_def->kind == EXPORT_KIND_INSTANCE); break;
                // EXTERN_DESC_KIND_MODULE is not directly exportable from a component, but from a core instance.
                default: kind_match = false;
            }
            if (!kind_match) {
                 set_comp_rt_error_v(error_buf, error_buf_size,
                                   "Export '%s' kind mismatch. Expected desc kind %d, actual export kind %d.",
                                   expected_export_decl->name, expected_export_decl->desc.kind, actual_export_def->kind);
                return false;
            }

            // For detailed type checking, we need to resolve the actual export's type.
            // actual_export_def->optional_desc_type_idx points to a WASMComponentDefinedType in actual_comp_def.
            // We need to construct a temporary WASMComponentExternDesc from this to use extern_desc_compatible,
            // or adapt extern_desc_compatible, or do direct specific checks.
            // Baseline: If actual_export_def->optional_desc_type_idx is valid, use it.
            if (actual_export_def->optional_desc_type_idx != (uint32)-1) {
                if (actual_export_def->optional_desc_type_idx >= actual_comp_def->type_definition_count) {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': actual export's type annotation index %u is out of bounds.",
                                       actual_export_def->name, actual_export_def->optional_desc_type_idx);
                    return false;
                }
                WASMComponentDefinedType *actual_export_type_def = &actual_comp_def->type_definitions[actual_export_def->optional_desc_type_idx];

                // Now, compare expected_export_decl->desc with actual_export_type_def.
                // This requires a new helper or enhancing extern_desc_compatible.
                // e.g., extern_desc_compatible_with_defined_type(expected_desc, actual_defined_type, ...)
                LOG_TODO("Detailed type check for export '%s' using its optional_desc_type_idx.", actual_export_def->name);

                // Simplified check for now: if expected is func, actual type def must be func, then compare func types.
                if (expected_export_decl->desc.kind == EXTERN_DESC_KIND_FUNC) {
                    if (actual_export_type_def->kind != DEF_TYPE_KIND_FUNC) {
                        set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': type kind mismatch. Expected func, actual def kind %d.", actual_export_def->name, actual_export_type_def->kind);
                        return false;
                    }
                    // Resolve expected func type
                     if (expected_export_decl->desc.u.func_type_idx >= defining_context_for_expected_type->type_definition_count
                         || defining_context_for_expected_type->type_definitions[expected_export_decl->desc.u.func_type_idx].kind != DEF_TYPE_KIND_FUNC) {
                         set_comp_rt_error(error_buf, error_buf_size, "Invalid expected function type index for export.");
                         return false;
                     }
                    WASMComponentFuncType *expected_ft = &defining_context_for_expected_type->type_definitions[expected_export_decl->desc.u.func_type_idx].u.func_type;
                    WASMComponentFuncType *actual_ft = &actual_export_type_def->u.func_type;
                    if (!component_func_type_compatible(expected_ft, actual_ft, defining_context_for_expected_type, actual_comp_def, error_buf, error_buf_size)) {
                        return false;
                    }
                }
                // Add similar blocks for other kinds (INSTANCE, COMPONENT, VALUE, TYPE)
            } else {
                // Actual export has no explicit type annotation. Subtyping might be more lenient or based on inference.
                // For a baseline, if expected has a specific type, this might be an incompatibility.
                // However, if expected_export_decl->desc is just a kind check without a specific type_idx, it might pass.
                LOG_TODO("Export '%s': Actual export has no type annotation. Compatibility check needs refinement.", actual_export_def->name);
            }
        }
    }
    return true;
}

// TODO: Implement this function
static bool
component_func_type_compatible(WASMComponentFuncType *expected_func_type,
                               WASMComponentFuncType *actual_func_type,
                               WASMComponent *expected_defining_component, /* Component that defines expected_func_type */
                               WASMComponent *actual_defining_component,   /* Component that defines actual_func_type */
                               char* error_buf, uint32 error_buf_size)
{
    LOG_TODO("component_func_type_compatible: Detailed check of params/results needed, including valtype_compatible calls.");
    return true;
}

// TODO: Implement this function
static bool
core_func_type_compatible_with_component_func_type(WASMType *expected_core_func_type,
                                                   WASMComponentFuncType *actual_comp_func_type,
                                                   WASMComponent *actual_defining_component, /* Component that defines actual_comp_func_type */
                                                   char* error_buf, uint32 error_buf_size)
{
    LOG_TODO("core_func_type_compatible_with_component_func_type: Detailed check needed.");
    return true;
}

// TODO: Implement this function
static bool
core_global_type_compatible_with_component_val_type(WASMComponentValType *expected_val_type, /* Defined in expected_defining_component */
                                                    WASMGlobalInstance *actual_core_global,
                                                    WASMComponent *expected_defining_component,
                                                    char* error_buf, uint32 error_buf_size)
{
    LOG_TODO("core_global_type_compatible_with_component_val_type: Detailed check needed.");
    return true;
}

// Placeholder for LiftedFuncThunk and create_lifted_function_thunk
// These would typically be in wasm_component_canonical.h/c or similar.
typedef struct LiftedFuncThunkContext {
    WASMComponentCanonical *canonical_def;
    WASMModuleInstance *target_core_module_inst;
    uint32 target_core_func_idx;
    WASMComponentFuncType *component_func_type;
    WASMExecEnv *parent_comp_exec_env;
    void *host_callable_c_function_ptr; // The actual C thunk
} LiftedFuncThunkContext;

// Conceptual function, actual implementation is part of Step 5's design
static LiftedFuncThunkContext*
create_lifted_function_thunk(WASMExecEnv *comp_exec_env,
                             WASMComponentCanonical *canonical_def,
                             WASMModuleInstance *target_core_inst, uint32 core_func_idx_in_mod,
                             WASMComponentFuncType *comp_func_type,
                             char *error_buf, uint32 error_buf_size)
{
    LOG_TODO("create_lifted_function_thunk: Full implementation needed based on Step 5 design.");
    // This function would:
    // 1. Allocate LiftedFuncThunkContext.
    // 2. Populate it with the provided details.
    // 3. Generate or assign a C function pointer to host_callable_c_function_ptr.
    //    This C function is the actual thunk that performs lowering/lifting.
    // For now, return a dummy or NULL.
    LiftedFuncThunkContext *thunk_ctx = bh_malloc(sizeof(LiftedFuncThunkContext));
    if (!thunk_ctx) {
        set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate LiftedFuncThunkContext");
        return NULL;
    }
    memset(thunk_ctx, 0, sizeof(LiftedFuncThunkContext));
    thunk_ctx->canonical_def = canonical_def;
    thunk_ctx->target_core_module_inst = target_core_inst;
    thunk_ctx->target_core_func_idx = core_func_idx_in_mod;
    thunk_ctx->component_func_type = comp_func_type;
    thunk_ctx->parent_comp_exec_env = comp_exec_env;
    // thunk_ctx->host_callable_c_function_ptr = some_generic_lifted_thunk_executor;

    // For this placeholder, we don't have the actual C thunk code.
    // In a real scenario, host_callable_c_function_ptr would point to a function
    // that uses the context to perform the call.
    set_comp_rt_error(error_buf, error_buf_size, "Lifted function thunk creation is a placeholder.");
    // Returning the context, but it's not fully functional without the C thunk ptr.
    return thunk_ctx; 
    // return NULL; // More realistic for a placeholder that can't create a working thunk
}


static bool execute_component_start_function(WASMComponentInstanceInternal *comp_inst, 
                                             WASMComponentStart *start_def, 
                                             char *error_buf, uint32 error_buf_size);


// Forward declaration for the new function
static bool wasm_component_instance_populate_exports(WASMComponentInstanceInternal *comp_inst, char *error_buf, uint32 error_buf_size);

static bool
wasm_component_instance_populate_exports(WASMComponentInstanceInternal *comp_inst, char *error_buf, uint32 error_buf_size)
{
    WASMComponent *component_def = comp_inst->component_def;
    uint32 i;

    if (component_def->export_count == 0) {
        comp_inst->resolved_exports = NULL;
        comp_inst->num_resolved_exports = 0;
        return true;
    }

    comp_inst->resolved_exports = bh_malloc(component_def->export_count * sizeof(ResolvedComponentExportItem));
    if (!comp_inst->resolved_exports) {
        set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate memory for resolved exports.");
        return false;
    }
    memset(comp_inst->resolved_exports, 0, component_def->export_count * sizeof(ResolvedComponentExportItem));
    comp_inst->num_resolved_exports = 0; // Will be incremented as each export is resolved

    for (i = 0; i < component_def->export_count; ++i) {
        WASMComponentExport *export_def = &component_def->exports[i];
        ResolvedComponentExportItem *resolved_export = &comp_inst->resolved_exports[comp_inst->num_resolved_exports];

        resolved_export->name = bh_strdup(export_def->name);
        if (!resolved_export->name) {
            set_comp_rt_error(error_buf, error_buf_size, "Failed to duplicate export name.");
            // Cleanup already strdup'd names in resolved_exports
            for (uint32 j = 0; j < comp_inst->num_resolved_exports; ++j) {
                if (comp_inst->resolved_exports[j].name) bh_free(comp_inst->resolved_exports[j].name);
            }
            bh_free(comp_inst->resolved_exports);
            comp_inst->resolved_exports = NULL;
            return false;
        }
        resolved_export->type_annotation_idx = export_def->optional_desc_type_idx;
        resolved_export->kind = (ResolvedComponentExportItemKind)export_def->kind;

        switch (export_def->kind) {
            case EXPORT_KIND_FUNC:
            {
                if (export_def->item_idx >= component_def->canonical_count) {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': func item_idx %u out of bounds for canonicals (count %u).", export_def->name, export_def->item_idx, component_def->canonical_count);
                    goto fail_export;
                }
                WASMComponentCanonical *canonical_def = &component_def->canonicals[export_def->item_idx];
                if (canonical_def->func_kind != CANONICAL_FUNC_KIND_LIFT) {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': func item_idx %u points to canonical of kind %u, expected LIFT.", export_def->name, export_def->item_idx, canonical_def->func_kind);
                    goto fail_export;
                }

                // Resolve component_func_type for the export
                if (canonical_def->u.lift.component_func_type_idx >= component_def->type_definition_count) {
                     set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': canonical lift func_type_idx %u out of bounds.", export_def->name, canonical_def->u.lift.component_func_type_idx);
                     goto fail_export;
                }
                WASMComponentDefinedType *func_type_def = &component_def->type_definitions[canonical_def->u.lift.component_func_type_idx];
                if (func_type_def->kind != DEF_TYPE_KIND_FUNC) {
                     set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': canonical lift func_type_idx %u points to non-func type def kind %u.", export_def->name, canonical_def->u.lift.component_func_type_idx, func_type_def->kind);
                     goto fail_export;
                }
                WASMComponentFuncType *comp_func_type = func_type_def->u.func_type;

                // TODO: Resolve target_core_inst and core_func_idx_in_mod from canonical_def->u.lift.core_func_idx
                // This part is complex: canonical_def->u.lift.core_func_idx might be an index into an alias list,
                // or an index into a flat list of all functions from all core modules.
                // For now, assume it's an index into comp_inst->component_def->core_instances, then an export name.
                // This needs the alias resolution logic from component spec: `(core func (instance <idx>) (export <name>))`
                // Let's assume for now the canonical definition has already resolved this to a specific core module instance definition index
                // and a function index *within that module*. This is a simplification.
                // The `WASMComponentCanonical.u.lift.core_func_idx` might more realistically be an index into `component_def->aliases`
                // which then needs to be resolved to a specific `core_instance_def_idx` and `func_idx_in_that_core_module`.
                // For this placeholder:
                LOG_TODO("Export '%s': Full resolution of canonical_def->u.lift.core_func_idx to target Wasm func needs alias/instance mapping.", export_def->name);
                // Placeholder: assume lift.core_func_idx is a direct index to a previously instantiated core module
                // and lift.options[0] (if memory/realloc) or some other field in canonical_def gives the func index within that module.
                // This is highly speculative and needs to be aligned with how the loader populates WASMComponentCanonical.
                // Let's assume for now that the lift definition refers to the Nth *function export* of the Mth *core instance definition*.
                // This is not robust. A proper alias resolution step during loading or here is needed.
                // For now, cannot fully implement without this resolution.
                set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Func export logic for resolving target core func not fully implemented.", export_def->name);
                goto fail_export;

                // Conceptual:
                // WASMModuleInstance* target_core_inst = comp_inst->module_instances[resolved_runtime_core_module_idx];
                // uint32 resolved_func_idx_in_core_module = ...;
                // resolved_export->item.function_thunk_context = create_lifted_function_thunk(
                //     comp_inst->exec_env, // Or parent_exec_env if that's the owner context for the thunk
                //     canonical_def,
                //     target_core_inst, resolved_func_idx_in_core_module,
                //     comp_func_type,
                //     error_buf, error_buf_size);
                // if (!resolved_export->item.function_thunk_context) {
                //     goto fail_export;
                // }
                // TODO: Type validation against export_def->optional_desc_type_idx
                break;
            }
            case EXPORT_KIND_INSTANCE:
            case EXPORT_KIND_COMPONENT: // Treat component export similar to instance export for now
            {
                // item_idx points to an instance definition in component_def
                // This could be a nested component instance or a core module instance.
                // The component model spec needs to clarify how core module instances are typed when exported as "instances".
                
                // Assuming item_idx refers to component_def->component_instances
                // This means we are exporting a nested component instance.
                if (export_def->item_idx >= component_def->component_instance_count) {
                     set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': instance item_idx %u out of bounds for component_instances (count %u).", export_def->name, export_def->item_idx, component_def->component_instance_count);
                     goto fail_export;
                }
                // The item_idx in WASMComponentExport refers to the definition-time index of the instance.
                // We need to find the corresponding runtime WASMComponentInstanceInternal*.
                // This requires a mapping from definition index to runtime index, similar to core_instance_map.
                // Let's assume such a map `nested_comp_inst_map` exists or that runtime indices match definition indices
                // if all defined instances are instantiated.
                // For now, assume direct mapping if comp_inst->component_instances[export_def->item_idx] is valid.
                // This implies that the component_instances array in WASMComponentInstanceInternal is indexed
                // by the *definition-time* index from WASMComponent.component_instances.
                // This needs to be consistent with how `current_runtime_comp_idx` is used.
                // `current_runtime_comp_idx` tracks truly instantiated components. A map is better.
                LOG_TODO("Export '%s': Mapping export_def->item_idx for instances to runtime comp_inst->component_instances index needs a map.", export_def->name);
                // Placeholder: Assume direct mapping for now if it's within num_component_instances
                if (export_def->item_idx < comp_inst->num_component_instances && 
                    component_def->component_instances[export_def->item_idx].kind == COMPONENT_INSTANCE_KIND_INSTANTIATE) {
                    // This check is insufficient without a map. The runtime index is not necessarily export_def->item_idx.
                    // For now, this will likely fail or point to wrong instance if not all are instantiated.
                    // We need to search comp_inst->component_instances for the one corresponding to component_def->component_instances[export_def->item_idx]
                    // This requires more context, perhaps storing definition index in WASMComponentInstanceInternal.
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Instance export logic for finding runtime instance not fully implemented.", export_def->name);
                    goto fail_export;
                    // resolved_export->item.instance = comp_inst->component_instances[runtime_idx_for_this_def_idx];
                } else {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': instance item_idx %u refers to non-instantiated or out-of-bounds component instance.", export_def->name, export_def->item_idx);
                    goto fail_export;
                }
                // TODO: Type validation against export_def->optional_desc_type_idx (should be an instance or component type)
                break;
            }
            case EXPORT_KIND_TYPE:
            {
                if (export_def->item_idx >= component_def->type_definition_count) {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': type item_idx %u out of bounds for type_definitions (count %u).", export_def->name, export_def->item_idx, component_def->type_definition_count);
                    goto fail_export;
                }
                resolved_export->item.type_definition = &component_def->type_definitions[export_def->item_idx];
                // No specific type validation needed here beyond bounds check, as it *is* the type.
                break;
            }
            case EXPORT_KIND_VALUE:
                LOG_TODO("Export '%s': Exporting values not yet implemented pending Value Section parsing and resolution.", export_def->name);
                // Placeholder:
                // resolved_export->item.value_ptr = resolve_value_from_idx(comp_inst, export_def->item_idx, error_buf, error_buf_size);
                // if (!resolved_export->item.value_ptr) goto fail_export;
                // TODO: Type validation against export_def->optional_desc_type_idx
                set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Value export not implemented.", export_def->name);
                goto fail_export; // For now, treat as error until implemented
            default:
                set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Unknown export kind %u.", export_def->name, export_def->kind);
                goto fail_export;
        }
        comp_inst->num_resolved_exports++;
    }

    return true;

fail_export:
    // Cleanup for the export item that failed and any previous successfully resolved items in this loop
    if (resolved_export->name) { // If current item's name was strduped before failure
        bh_free(resolved_export->name);
        resolved_export->name = NULL; 
    }
    for (uint32 j = 0; j < comp_inst->num_resolved_exports; ++j) { // num_resolved_exports is count of *successful* ones
        if (comp_inst->resolved_exports[j].name) bh_free(comp_inst->resolved_exports[j].name);
        // TODO: Free other allocated items like function_thunk_context if any were created
    }
    bh_free(comp_inst->resolved_exports);
    comp_inst->resolved_exports = NULL;
    comp_inst->num_resolved_exports = 0;
    return false;
}

static bool
execute_component_start_function(WASMComponentInstanceInternal *comp_inst, 
                                 WASMComponentStart *start_def, 
                                 char *error_buf, uint32 error_buf_size)
{
    WASMComponent *component_def = comp_inst->component_def;
    ResolvedComponentExportItem *target_export_func = NULL;
    WASMComponentFuncType *func_type = NULL; // Component function type of the start function

    // 1. Resolve Target Function:
    // start_def->func_idx is an index into the component's function index space.
    // This means it's an index into items that *define* functions,
    // which are typically `canon` section items or aliases to functions.
    // Exports also refer to these canonical definitions by item_idx.
    // So, find the export that corresponds to this canonical definition.
    
    // Find the export that refers to the same canonical item as start_def->func_idx
    // (assuming start_def->func_idx directly maps to an item_idx in the exports list that is a function)
    // This is a simplification. A more robust way would be to map start_def->func_idx
    // to a canonical definition, then find an export that also maps to that same canonical def.
    // Or, the runtime could have a flat list of all "callable" component functions (lifted thunks).
    bool found_export = false;
    for (uint32 i = 0; i < comp_inst->num_resolved_exports; ++i) {
        // We need to know the original definition index of the export's item.
        // WASMComponentExport has `item_idx` which points to the canonical def.
        // We need to find which `comp_inst->resolved_exports[i]` corresponds to `component_def->exports[k]`
        // where `component_def->exports[k].item_idx == start_def->func_idx` and `exports[k].kind == EXPORT_KIND_FUNC`.
        // This direct iteration over resolved_exports might not be enough if names don't match or if func_idx is not an export name.
        // Let's assume for now start_def->func_idx is an index into the `component_def->canonicals` array
        // and that the export we are looking for also points to this same canonical definition.
        
        // Find the original export definition that matches this resolved export
        WASMComponentExport *original_export_def = NULL;
        for (uint32 exp_idx = 0; exp_idx < component_def->export_count; ++exp_idx) {
            if (strcmp(component_def->exports[exp_idx].name, comp_inst->resolved_exports[i].name) == 0 &&
                component_def->exports[exp_idx].kind == EXPORT_KIND_FUNC) { // Ensure it's a function export
                original_export_def = &component_def->exports[exp_idx];
                if (original_export_def->item_idx == start_def->func_idx) { // Check if this export's item_idx matches func_idx
                    target_export_func = &comp_inst->resolved_exports[i];
                    found_export = true;
                    break;
                }
            }
        }
        if (found_export) break;
    }


    if (!target_export_func) {
        set_comp_rt_error_v(error_buf, error_buf_size, "Start function with index %u not found in resolved exports or not a function.", start_def->func_idx);
        return false;
    }

    if (target_export_func->kind != COMPONENT_EXPORT_KIND_FUNC || !target_export_func->item.function_thunk_context) {
        set_comp_rt_error_v(error_buf, error_buf_size, "Resolved start function export '%s' is not a callable function thunk.", target_export_func->name);
        return false;
    }

    LiftedFuncThunkContext *thunk_ctx = (LiftedFuncThunkContext*)target_export_func->item.function_thunk_context;
    func_type = thunk_ctx->component_func_type; // Get the WALAComponentFuncType from the thunk context.

    if (!func_type) { // Should be set during thunk creation
        set_comp_rt_error_v(error_buf, error_buf_size, "Start function '%s' resolved but missing its component type information.", target_export_func->name);
        return false;
    }

    // Validate signature: must have no results.
    if (func_type->result != NULL) { // Assuming result is NULL or points to a ValType if there's a result.
                                     // A void result in ComponentFuncType might have result as NULL.
        set_comp_rt_error_v(error_buf, error_buf_size, "Start function '%s' must have no results.", target_export_func->name);
        return false;
    }

    // 2. Resolve Arguments:
    void **component_argv = NULL;
    if (start_def->arg_count != func_type->param_count) {
        set_comp_rt_error_v(error_buf, error_buf_size,
                           "Start function '%s' argument count mismatch. Definition has %u, function type expects %u.",
                           target_export_func->name, start_def->arg_count, func_type->param_count);
        return false;
    }

    if (start_def->arg_count > 0) {
        uint32 i;
        component_argv = bh_malloc(start_def->arg_count * sizeof(void*));
        if (!component_argv) {
            set_comp_rt_error(error_buf, error_buf_size, "Memory allocation failed for start function arguments.");
            return false;
        }
        memset(component_argv, 0, start_def->arg_count * sizeof(void*));

        for (i = 0; i < start_def->arg_count; ++i) {
            uint32 value_idx = start_def->arg_value_indices[i];
            if (value_idx >= comp_inst->component_def->value_count) {
                set_comp_rt_error_v(error_buf, error_buf_size,
                                   "Start function '%s' argument %d: value_idx %u out of bounds for component values (count %u).",
                                   target_export_func->name, i, value_idx, comp_inst->component_def->value_count);
                bh_free(component_argv);
                return false;
            }
            WASMComponentValue *source_comp_value = &comp_inst->component_def->values[value_idx];
            WASMComponentValType *expected_arg_type = func_type->params[i].valtype; // Assuming params is an array of LabelValType

            if (!component_val_type_compatible(expected_arg_type, &source_comp_value->parsed_type,
                                               comp_inst->component_def, /* context for expected (func params are defined in this component) */
                                               comp_inst->component_def, /* context for actual (values are defined in this component) */
                                               error_buf, error_buf_size)) {
                // Prepend argument index to error message if not already specific enough
                // For now, component_val_type_compatible should set a good error.
                bh_free(component_argv);
                return false;
            }
            component_argv[i] = (void*)&source_comp_value->val;
        }
    }

    // 3. Invoke Function:
    LOG_VERBOSE("Executing component start function '%s' with %u arguments.", target_export_func->name, start_def->arg_count);

    if (!thunk_ctx->host_callable_c_function_ptr) { // Should be generic_thunk_executor_ptr or similar
         set_comp_rt_error_v(error_buf, error_buf_size, "Start function '%s' thunk context is missing the callable C function pointer.", target_export_func->name);
         if (component_argv) bh_free(component_argv);
         return false;
    }

    WASMExecEnv *exec_env_for_thunk = thunk_ctx->parent_comp_exec_env; 
    if (!exec_env_for_thunk) { 
        exec_env_for_thunk = comp_inst->exec_env; 
        if (!exec_env_for_thunk) {
             set_comp_rt_error_v(error_buf, error_buf_size, "No valid WASMExecEnv found for executing start function '%s'.", target_export_func->name);
             if (component_argv) bh_free(component_argv);
             return false;
        }
    }
    
    // Assuming the generic thunk executor has a signature like:
    // bool generic_thunk_executor(WASMExecEnv *exec_env, LiftedFuncThunkContext *thunk_context, 
    //                             uint32 argc, void **argv, void **results_ptr_array);
    // And it returns true on success, false on trap/exception (setting error_buf).
    // The host_callable_c_function_ptr in thunk_ctx should point to such a generic executor.
    typedef bool (*generic_thunk_executor_t)(WASMExecEnv*, LiftedFuncThunkContext*, uint32, void**, void**);
    generic_thunk_executor_t executor = (generic_thunk_executor_t)thunk_ctx->host_callable_c_function_ptr;

    // For start function, results_ptr_array is NULL.
    bool success = executor(exec_env_for_thunk, thunk_ctx, start_def->arg_count, component_argv, NULL);

    if (component_argv) {
        bh_free(component_argv);
    }

    if (!success) {
        // error_buf should have been set by the executor or by canonical ABI functions it called.
        // If not, set a generic one.
        if (error_buf[0] == '\0') {
             set_comp_rt_error_v(error_buf, error_buf_size, "Start function '%s' execution failed or trapped.", target_export_func->name);
        }
        return false;
    }
    
    // Check for exceptions if the call were made (already handled by generic_thunk_executor ideally)
    // WASMModuleInstance *exception_module_inst = wasm_runtime_get_module_inst(exec_env_for_thunk); // Which module instance to check?
    // if (exception_module_inst && wasm_runtime_get_exception(exception_module_inst)) {
    //     set_comp_rt_error_v(error_buf, error_buf_size, "Exception occurred during start function '%s': %s",
    //                        target_export_func->name, wasm_runtime_get_exception(exception_module_inst));
    //     wasm_runtime_clear_exception(exception_module_inst);
    //     return false;
    // }

    return true;
}
