/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_component_runtime.h"
#include "wasm_component_loader.h" /* For WASMComponent structure details */
#include "wasm_runtime.h"          /* For wasm_instantiate, wasm_deinstantiate */
#include "wasm_loader.h"           /* For wasm_module_destroy (if needed for component def) */
#include "../common/bh_log.h"
#include "../common/bh_platform.h" /* For bh_malloc, bh_free, memset */
#include "wasm_memory.h"        /* For wasm_runtime_validate_app_addr, etc. */


// Helper to set component runtime error messages
static void
set_comp_rt_error(char *error_buf, uint32 error_buf_size, const char *message); // Keep FWD decl

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
                                char *error_buf, uint32 error_buf_size) // TODO: check mutability
{
    for (uint32 i = 0; i < module_inst->export_global_count; ++i) {
        if (strcmp(module_inst->export_globals[i].name, name) == 0) {
            // TODO: Check module_inst->export_globals[i].is_mutable against is_mutable from import
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
                        // WASMImportKind vs WASMComponentExternKind (from matched_arg->kind.item_kind)
                        bool kind_compatible = false;
                        switch (import_def->kind) {
                            case WASM_IMPORT_KIND_FUNC:
                                kind_compatible = (matched_arg->kind.item_kind == COMPONENT_ITEM_KIND_FUNC);
                                break;
                            case WASM_IMPORT_KIND_TABLE:
                                kind_compatible = (matched_arg->kind.item_kind == COMPONENT_ITEM_KIND_TABLE);
                                break;
                            case WASM_IMPORT_KIND_MEMORY:
                                kind_compatible = (matched_arg->kind.item_kind == COMPONENT_ITEM_KIND_MEMORY);
                                break;
                            case WASM_IMPORT_KIND_GLOBAL:
                                kind_compatible = (matched_arg->kind.item_kind == COMPONENT_ITEM_KIND_GLOBAL);
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
                                       inst_args[arg_k].name, inst_args[arg_k].kind.item_kind);
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
                                        // TODO: Determine is_native_func and call_conv_raw correctly for host funcs
                                        resolved_func_imports[current_func_import_k].is_native_func = false; // Revisit this
                                        resolved_func_imports[current_func_import_k].call_conv_raw = false;
                                        current_func_import_k++;
                                        break;
                                    case WASM_IMPORT_KIND_GLOBAL:
                                        // Similar assumptions for globals, memories, tables
                                        resolved_global_imports[current_global_import_k].module_name = (char*)import_def->module_name;
                                        resolved_global_imports[current_global_import_k].field_name = (char*)import_def->field_name;
                                        resolved_global_imports[current_global_import_k].global_ptr_linked = comp_inst_internal->resolved_imports[k].item.global;
                                        resolved_global_imports[current_global_import_k].is_linked = true;
                                        current_global_import_k++;
                                        break;
                                    // TODO: Handle TABLE and MEMORY from component imports
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
            uint32 nested_resolved_idx = 0; // Index for populating nested_imports_resolved

            for (uint32 arg_idx = 0; arg_idx < comp_instance_def->u.instantiate.arg_count; ++arg_idx) {
                WASMComponentCompInstanceArg *arg = &comp_instance_def->u.instantiate.args[arg_idx];
                // 'arg->name' is the import name for the nested component.
                // 'arg->instance_idx' is the index in the outer component's instance space.
                // 'arg->kind.item_kind' is the kind of item expected by the nested component.

            bool nested_import_res_failed = false;
            uint32 nested_resolved_idx = 0; // Index for populating nested_imports_resolved

            // The WASMComponent structure should provide distinct counts for imports, core instances, etc.
            // Let's assume:
            // component->import_count = number of component-level imports
            // component->core_instance_count = number of core instance definitions
            // component->component_instance_count = number of nested component instance definitions
            // The arg->instance_idx needs to be interpreted against these.
            // A common convention is that instance_idx refers to a flat array of all possible
            // "definable" items in the component: imports first, then locally defined instances.

            for (uint32 arg_k = 0; arg_k < comp_instance_def->u.instantiate.arg_count; ++arg_k) {
                WASMComponentCompInstanceArg *arg = &comp_instance_def->u.instantiate.args[arg_k];
                uint32 source_instance_index = arg->instance_idx; // This is the key index to resolve

                LOG_VERBOSE("Resolving arg '%s' for nested component, source index %u in outer, kind %u", 
                            arg->name, source_instance_index, arg->kind.item_kind);

                // TODO: The component loader should provide a clear mapping from instance_idx to item type (import, local core instance, local component instance)
                // For now, make a simplifying assumption on index ranges.
                // This part needs to be robust based on how instance_idx is defined by the loader.
                // Assume instance_idx maps to a global "instance space" for the component:
                // [component_imports] [core_module_instances] [nested_component_instances] ... and others (types, etc.)

                // This logic assumes a flat index space for instances as defined in the component:
                // 0 to import_count - 1                            : imports of the current component
                // import_count to import_count + core_instance_count - 1 : core instances defined in the current component
                // import_count + core_instance_count to ...       : component instances defined in the current component
                // This needs to be robustly defined by the loader via component->instance_lookup_map or similar.

                WASMComponentImport *outer_component_imports = comp_inst_internal->component_def->imports;
                uint32 num_outer_component_imports = comp_inst_internal->component_def->import_count;
                uint32 num_outer_core_instance_defs = comp_inst_internal->component_def->core_instance_count;
                // uint32 num_outer_comp_instance_defs = comp_inst_internal->component_def->component_instance_count;


                if (source_instance_index < num_outer_component_imports) {
                    // Source is an import of the current (outer) component.
                    // The arg->name is the name the NESTED component expects for its import.
                    // The component->imports[source_instance_index].name is the name of the import for the OUTER component.
                    // We need to find the ResolvedComponentImportItem that was passed to the *outer* component's instantiation
                    // that matches component->imports[source_instance_index].name.
                    WASMComponentImport *outer_import_def = &outer_component_imports[source_instance_index];
                    ResolvedComponentImportItem *provided_outer_import = NULL;

                    for (uint32 k = 0; k < comp_inst_internal->num_resolved_imports; ++k) {
                        if (strcmp(comp_inst_internal->resolved_imports[k].name, outer_import_def->name) == 0) {
                            provided_outer_import = &comp_inst_internal->resolved_imports[k];
                            break;
                        }
                    }

                    if (!provided_outer_import) {
                        set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': required outer component import '%s' (def_idx %u) was not resolved/provided to outer component.",
                                           arg->name, outer_import_def->name, source_instance_index);
                        nested_import_res_failed = true;
                    } else if (provided_outer_import->kind != arg->kind.item_kind) {
                        set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': kind mismatch. Expected %u, outer import '%s' provides %u.",
                                           arg->name, arg->kind.item_kind, outer_import_def->name, provided_outer_import->kind);
                        nested_import_res_failed = true;
                    } else {
                        nested_imports_resolved[nested_resolved_idx].name = bh_strdup(arg->name); // Name for the nested component's import
                        if (!nested_imports_resolved[nested_resolved_idx].name) { nested_import_res_failed = true; }
                        else {
                            nested_imports_resolved[nested_resolved_idx].kind = provided_outer_import->kind;
                            nested_imports_resolved[nested_resolved_idx].item = provided_outer_import->item; // Shallow copy of runtime item
                            nested_resolved_idx++;
                        }
                    }
                } else if (source_instance_index < num_outer_component_imports + num_outer_core_instance_defs) {
                    uint32 core_instance_def_idx = source_instance_index - num_outer_component_imports;
                    uint32 src_runtime_mod_arr_idx = comp_inst_internal->core_instance_map[core_instance_def_idx];

                    if (src_runtime_mod_arr_idx == (uint32)-1) {
                        set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': source core instance def %u is an inline export or not directly instantiated.", arg->name, core_instance_def_idx);
                        nested_import_res_failed = true;
                    } else {
                        WASMModuleInstance *src_mod_inst = comp_inst_internal->module_instances[src_runtime_mod_arr_idx];
                        if (!src_mod_inst) {
                            set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': source core module instance (def_idx %u, runtime_idx %u) is NULL.",
                                               arg->name, core_instance_def_idx, src_runtime_mod_arr_idx);
                            nested_import_res_failed = true;
                        } else {
                            // The 'name' in WASMComponentCompInstanceArg (arg->name) is the import name for the nested component.
                            // The actual export name from src_mod_inst might be different if there are aliases.
                            // For now, assume arg->name IS the export name to look up in src_mod_inst.
                            // This mapping needs to be correctly derived from component definition (e.g. aliases).
                            const char *export_name_from_source = arg->name; 
                                                        
                            nested_imports_resolved[nested_resolved_idx].name = bh_strdup(arg->name);
                            if (!nested_imports_resolved[nested_resolved_idx].name) { nested_import_res_failed = true; }
                            else {
                                nested_imports_resolved[nested_resolved_idx].kind = arg->kind.item_kind;
                                switch (arg->kind.item_kind) {
                                    case COMPONENT_ITEM_KIND_FUNC:
                                        nested_imports_resolved[nested_resolved_idx].item.function = find_exported_function_instance(src_mod_inst, export_name_from_source, error_buf, error_buf_size);
                                        if (!nested_imports_resolved[nested_resolved_idx].item.function) { nested_import_res_failed = true; }
                                        break;
                                    case COMPONENT_ITEM_KIND_GLOBAL:
                                        nested_imports_resolved[nested_resolved_idx].item.global = find_exported_global_instance(src_mod_inst, export_name_from_source, arg->kind.u.global.is_mutable, error_buf, error_buf_size);
                                        if (!nested_imports_resolved[nested_resolved_idx].item.global) { nested_import_res_failed = true; }
                                        break;
                                    case COMPONENT_ITEM_KIND_TABLE:
                                        nested_imports_resolved[nested_resolved_idx].item.table = find_exported_table_instance(src_mod_inst, export_name_from_source, error_buf, error_buf_size);
                                        if (!nested_imports_resolved[nested_resolved_idx].item.table) { nested_import_res_failed = true; }
                                        break;
                                    case COMPONENT_ITEM_KIND_MEMORY:
                                        nested_imports_resolved[nested_resolved_idx].item.memory = find_exported_memory_instance(src_mod_inst, export_name_from_source, error_buf, error_buf_size);
                                        if (!nested_imports_resolved[nested_resolved_idx].item.memory) { nested_import_res_failed = true; }
                                        break;
                                    default:
                                        set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': unhandled item kind %u for core module source.", arg->name, arg->kind.item_kind);
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
                // TODO: Handle source being another nested component instance
                // else if (source_instance_index < num_outer_component_imports + num_outer_core_instance_defs + num_outer_comp_instance_defs) { ... }
                else {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Nested comp arg '%s': source instance index %u out of supported range or unhandled type.", arg->name, source_instance_index);
                    nested_import_res_failed = true;
                }
                if (nested_import_res_failed) break; // Break from args loop
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


    // TODO: Populate Component Exports
    // Iterate component->exports.
    // For each export, find the actual item (e.g., function from a module_instances[idx] or component_instances[idx]).
    // If it's a function, potentially create a lifted thunk using canonical definitions.
    // Store in a yet-to-be-defined comp_inst_internal->exports_map.
    LOG_TODO("Component export population not implemented.");

    // TODO: Handle Start Function
    // If component->starts is defined, resolve the start function and its arguments.
    // Arguments might come from various instantiated instances.
    // Then call the start function.
    LOG_TODO("Component start function execution not implemented.");


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
    
    if (comp_inst->core_instance_map) {
        bh_free(comp_inst->core_instance_map);
    }

    bh_free(comp_inst);
}
