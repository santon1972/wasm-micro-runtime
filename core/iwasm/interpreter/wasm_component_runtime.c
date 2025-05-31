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
                                char *error_buf, uint32 error_buf_size)
{
    for (uint32 i = 0; i < module_inst->export_global_count; ++i) {
        if (strcmp(module_inst->export_globals[i].name, name) == 0) {
            WASMGlobalInstance *found_global = module_inst->export_globals[i].global;
            if (found_global->is_mutable != is_mutable) {
                set_comp_rt_error_v(error_buf, error_buf_size,
                                   "Global export '%s' found, but mutability mismatch (expected %s, got %s).",
                                   name, is_mutable ? "mutable" : "immutable",
                                   found_global->is_mutable ? "mutable" : "immutable");
                return NULL;
            }
            return found_global;
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
    char buf[256]; // Increased buffer size

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
                        // Check kind compatibility for core module imports.
                        // import_def->kind is WASMImportKind (0x00=func, 0x01=table, etc.) from the core module's definition.
                        // inst_args[arg_k].kind (matched_arg->kind) is populated by the loader from the
                        // component binary's `core instance` section. The `sort_byte` used there
                        // for item kinds directly corresponds to WASMImportKind values.
                        // Therefore, a direct comparison is correct.
                        if (import_def->kind == inst_args[arg_k].kind) {
                            matched_arg = &inst_args[arg_k];
                            break;
                        } else {
                            LOG_VERBOSE("Import '%s':'%s' (expected core kind %u) not satisfied by arg '%s' (provided core kind %u) due to kind mismatch.",
                                       import_def->module_name, import_def->field_name, import_def->kind,
                                       inst_args[arg_k].name, inst_args[arg_k].kind);
                            // Continue searching other args, maybe another arg with same name has correct kind.
                            // Note: The original code incorrectly compared against COMPONENT_ITEM_KIND_*.
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
                                            resolved_func_imports[current_func_import_k].is_native_func = resolved_func->is_native_func;
                                            resolved_func_imports[current_func_import_k].call_conv_raw = resolved_func->call_conv_raw;
                                            // Note: attachment and other fields might need to be copied if relevant for host funcs.
                                        } else {
                                            // Should not happen if kind matches and item is set. Defaulting defensively.
                                            resolved_func_imports[current_func_import_k].is_native_func = false;
                                            resolved_func_imports[current_func_import_k].call_conv_raw = false;
                                        }
                                        current_func_import_k++;
                                        break;
                                    case WASM_IMPORT_KIND_GLOBAL:
                                        resolved_global_imports[current_global_import_k].module_name = (char*)import_def->module_name;
                                        resolved_global_imports[current_global_import_k].field_name = (char*)import_def->field_name;
                                        resolved_global_imports[current_global_import_k].global_ptr_linked = comp_inst_internal->resolved_imports[k].item.global;
                                        // Type and mutability check for globals from component imports
                                        if (comp_inst_internal->resolved_imports[k].item.global->type != import_def->u.global.type ||
                                            comp_inst_internal->resolved_imports[k].item.global->is_mutable != import_def->u.global.is_mutable) {
                                            set_comp_rt_error_v(error_buf, error_buf_size, "Global import '%s' (from component import) type or mutability mismatch.", import_def->field_name);
                                            import_resolution_failed = true; break;
                                        }
                                        resolved_global_imports[current_global_import_k].is_linked = true;
                                        current_global_import_k++;
                                        break;
                                    case WASM_IMPORT_KIND_TABLE:
                                    {
                                        WASMTableInstance *resolved_table = comp_inst_internal->resolved_imports[k].item.table;
                                        if (import_def->u.table.elem_type != resolved_table->elem_type) {
                                            set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' (from component import) element type mismatch.", import_def->field_name);
                                            import_resolution_failed = true; break;
                                        }
                                        if (resolved_table->init_size < import_def->u.table.init_size) {
                                            set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' (from component import) initial size too small.", import_def->field_name);
                                            import_resolution_failed = true; break;
                                        }
                                        if (import_def->u.table.has_max_size) {
                                            if (!resolved_table->has_max_size) {
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' (from component import) expects max size, but export has no max.", import_def->field_name);
                                                import_resolution_failed = true; break;
                                            }
                                            if (resolved_table->max_size > import_def->u.table.max_size) {
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Table import '%s' (from component import) max size too large.", import_def->field_name);
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
                                        if (resolved_memory->init_page_count < import_def->u.memory.init_page_count) {
                                            set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' (from component import) initial pages too small.", import_def->field_name);
                                            import_resolution_failed = true; break;
                                        }
                                        if (import_def->u.memory.has_max_size) {
                                            if (resolved_memory->max_page_count == 0) { // max_page_count is 0 if no max
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' (from component import) expects max pages, but export has no max.", import_def->field_name);
                                                import_resolution_failed = true; break;
                                            }
                                            if (resolved_memory->max_page_count > import_def->u.memory.max_page_count) {
                                                set_comp_rt_error_v(error_buf, error_buf_size, "Memory import '%s' (from component import) max pages too large.", import_def->field_name);
                                                import_resolution_failed = true; break;
                                            }
                                        }
                                        // TODO: Check shared memory flags if/when supported.
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
                switch (arg->kind.item_kind) { // This is WASMComponentItemKind from the outer component's context
                    case COMPONENT_ITEM_KIND_FUNC: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_FUNC); break;
                    case COMPONENT_ITEM_KIND_GLOBAL: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_VALUE); break;
                    case COMPONENT_ITEM_KIND_TABLE:
                        LOG_WARNING("Component-level import of TABLE ('%s') is not supported.", arg->name);
                        set_comp_rt_error_v(error_buf, error_buf_size, "Component-level import of TABLE ('%s') is not supported.", arg->name);
                        nested_import_res_failed = true; // Ensure failure path is taken
                        basic_kind_compatible = false; // Explicitly not compatible
                        break;
                    case COMPONENT_ITEM_KIND_MEMORY:
                        LOG_WARNING("Component-level import of MEMORY ('%s') is not supported.", arg->name);
                        set_comp_rt_error_v(error_buf, error_buf_size, "Component-level import of MEMORY ('%s') is not supported.", arg->name);
                        nested_import_res_failed = true; // Ensure failure path is taken
                        basic_kind_compatible = false; // Explicitly not compatible
                        break;
                    case COMPONENT_ITEM_KIND_MODULE: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_MODULE); break;
                    case COMPONENT_ITEM_KIND_COMPONENT: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_COMPONENT); break;
                    case COMPONENT_ITEM_KIND_INSTANCE: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_INSTANCE); break;
                    case COMPONENT_ITEM_KIND_TYPE: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_TYPE); break;
                    case COMPONENT_ITEM_KIND_VALUE: basic_kind_compatible = (nested_import_def->desc.kind == EXTERN_DESC_KIND_VALUE); break;
                    default:
                        LOG_WARNING("Unsupported item_kind %d encountered during nested component import kind compatibility check.", arg->kind.item_kind);
                        basic_kind_compatible = false;
                        break;
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
// Placeholder for LiftedFuncThunk and create_lifted_function_thunk
// These would typically be in wasm_component_canonical.h/c or similar.

// Forward declaration for generic_lifted_thunk_executor
static bool
generic_lifted_thunk_executor(WASMExecEnv *exec_env, uint32 argc, uint32 argv[]);


typedef struct LiftedFuncThunkContext {
    WASMComponentCanonical *canonical_def;
    WASMModuleInstance *target_core_module_inst;
    uint32 target_core_func_idx; /* Function index within the target_core_module_inst's function space */
    WASMComponentFuncType *component_func_type; /* The component-level type of this lifted function */
    WASMExecEnv *parent_comp_exec_env; /* Exec env of the component instance owning this thunk */
    WASMComponentInstanceInternal *owning_comp_inst; /* Component instance that owns this lifted function */
    void *host_callable_c_function_ptr; // The actual C thunk (points to generic_lifted_thunk_executor)
} LiftedFuncThunkContext;

// Conceptual function, actual implementation is part of Step 5's design
static LiftedFuncThunkContext*
create_lifted_function_thunk(WASMExecEnv *comp_exec_env, /* Exec env of the component instance being populated */
                             WASMComponentInstanceInternal *owning_component_instance, /* The component instance that will own this thunk */
                             WASMComponentCanonical *canonical_def,
                             WASMModuleInstance *target_core_inst, uint32 core_func_idx_in_mod,
                             WASMComponentFuncType *comp_func_type,
                             char *error_buf, uint32 error_buf_size)
{
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
    thunk_ctx->owning_comp_inst = owning_component_instance;
    thunk_ctx->host_callable_c_function_ptr = (void *)generic_lifted_thunk_executor;

    LOG_VERBOSE("Created lifted function thunk context %p for canonical_def %p, target_core_inst %p, core_func_idx %u, comp_func_type %p, owning_comp_inst %p",
                (void*)thunk_ctx, (void*)canonical_def, (void*)target_core_inst, core_func_idx_in_mod, (void*)comp_func_type, (void*)owning_component_instance);

    return thunk_ctx;
}

static bool
generic_lifted_thunk_executor(WASMExecEnv *exec_env, uint32 host_argc, uint32 host_argv_raw[])
{
    // exec_env is the calling exec_env, which might be the component's or a host exec_env.
    // The actual LiftedFuncThunkContext is the first argument effectively.
    // The host_callable_c_function_ptr in LiftedFuncThunkContext points here.
    // WAMR's native call mechanism passes the context as the first element of argv if it's a bound function.
    // However, this executor is usually called via wasm_runtime_invoke_c_function or similar,
    // where the `argc` and `argv` are derived from the component call interface.
    // The `LiftedFuncThunkContext` needs to be retrieved.
    // For now, assume this function is called via a mechanism where `exec_env->current_thunk_context` is set.
    // This needs alignment with how WAMR invokes these thunks (e.g. via `wasm_runtime_invoke_c_function_ex`).
    // If this is registered as a native function, the first arg in argv is the context.
    // For this subtask, let's assume argc/argv are component-level args/results.
    // The context needs to be passed differently, perhaps via exec_env or a convention.

    // This placeholder does not have access to LiftedFuncThunkContext, which is critical.
    // The function signature needs to be aligned with how it's called.
    // Let's assume for now it's called like a WAMR host function where the context is implicitly available
    // or passed in a standard way.
    // If this is directly set as host_callable_c_function_ptr, it needs to be a `bool (*)(WASMExecEnv*, uint32, uint32[])`
    // where the context comes from `exec_env->current_thunk_context` or similar.
    // OR, it's registered with `wasm_runtime_register_c_function` and context is first arg.

    // Re-evaluating: The prompt asks to refine this. It implies it exists.
    // The way native functions are called in WAMR:
    // If a function is registered with wasm_runtime_register_c_function, its signature is `bool func(WASMExecEnv *exec_env, uint32_t argc, uint32_t argv[])`
    // The context is usually passed as the first element in argv (argv[0]) IF the registration mechanism supports it (e.g. by using a wrapper).
    // Or, the context is attached to the WASMFunctionInstance somehow.
    // Given LiftedFuncThunkContext.host_callable_c_function_ptr, this executor is the target.
    // Let's assume the context is argv[0] and actual args follow.
    // This means the real argc is host_argc-1, and args start at host_argv_raw[1].
    // This is a common WAMR pattern for contextful C functions.

    if (host_argc == 0) {
        LOG_ERROR("GenericLiftedThunk: Thunk context expected as first argument, but argc is 0.");
        // wasm_runtime_set_exception(wasm_runtime_get_module_inst(exec_env), "GenericLiftedThunk: Missing context");
        return false; // Indicate failure
    }

    LiftedFuncThunkContext *thunk_ctx = (LiftedFuncThunkContext *)((uintptr_t)host_argv_raw[0]);
    if (!thunk_ctx || !thunk_ctx->canonical_def || !thunk_ctx->target_core_module_inst || !thunk_ctx->component_func_type) {
        LOG_ERROR("GenericLiftedThunk: Invalid or incomplete thunk context received.");
        // wasm_runtime_set_exception with details
        return false;
    }

    WASMComponentInstanceInternal *owning_comp_inst = thunk_ctx->owning_comp_inst;
    if (!owning_comp_inst) {
        LOG_ERROR("GenericLiftedThunk: Owning component instance is NULL in thunk context.");
        return false;
    }
    // Use owning_comp_inst->exec_env for operations within the component's context if needed.
    // exec_env passed to this function is the caller's environment.
    WASMExecEnv *target_exec_env = thunk_ctx->target_core_module_inst->exec_env; // For calling the core Wasm function

    WASMComponentFuncType *comp_func_type = thunk_ctx->component_func_type;
    uint32 num_comp_params = comp_func_type->param_count;
    uint32 actual_host_argc = host_argc - 1; // First arg is context

    if (actual_host_argc != num_comp_params) {
        LOG_ERROR("GenericLiftedThunk: Argument count mismatch. Expected %u component params, got %u host args (after context).",
                  num_comp_params, actual_host_argc);
        // wasm_runtime_set_exception
        return false;
    }

    // Prepare core_argv for wasm_runtime_call_wasm
    // This needs to be dynamically sized based on the core function's signature.
    WASMFunctionInstance *core_func_resolved = wasm_get_function(thunk_ctx->target_core_module_inst, thunk_ctx->target_core_func_idx);
    if (!core_func_resolved) {
        LOG_ERROR("GenericLiftedThunk: Failed to resolve target core function index %u.", thunk_ctx->target_core_func_idx);
        return false;
    }
    WASMType *core_func_wasm_type = core_func_resolved->u.func->func_type;
    uint32 core_param_count = core_func_wasm_type->param_count;
    uint32 core_result_count = core_func_wasm_type->result_count;
    uint32 total_core_cells = core_param_count + core_result_count; // Simplification, real cell count needed

    // Calculate actual cell numbers for params and results
    uint32 core_param_cell_num = 0;
    for (uint32 k=0; k < core_param_count; ++k) core_param_cell_num += wasm_value_type_cell_num(core_func_wasm_type->types[k]);
    uint32 core_result_cell_num = 0;
    for (uint32 k=0; k < core_result_count; ++k) core_result_cell_num += wasm_value_type_cell_num(core_func_wasm_type->types[core_param_count + k]);
    total_core_cells = core_param_cell_num + core_result_cell_num;


    uint32 *core_argv = NULL;
    if (total_core_cells > 0) {
        core_argv = bh_malloc(total_core_cells * sizeof(uint32));
        if (!core_argv) {
            LOG_ERROR("GenericLiftedThunk: Failed to allocate memory for core_argv.");
            return false;
        }
        memset(core_argv, 0, total_core_cells * sizeof(uint32));
    }

    char error_buf_internal[128]; // For canonical_lower/lift errors

    // 1. Argument Lowering Loop
    uint32 current_core_argv_idx = 0;
    for (uint32 i = 0; i < num_comp_params; ++i) {
        WASMComponentValType *param_comp_type = comp_func_type->params[i].valtype;
        void *host_arg_val_ptr = (void *)((uintptr_t)host_argv_raw[i + 1]); // +1 to skip context

        // Determine target_core_wasm_type for this param.
        // This is complex: needs mapping from param_comp_type to core Wasm type(s) via ABI details.
        // For now, assume wasm_component_canon_lower_value handles this determination internally
        // or we use a helper. get_component_type_core_abi_details can give us this.
        // For simplicity, let's assume it's mostly I32 for refs, or direct for primitives.
        // This part needs to be robust based on get_core_wasm_type_for_valtype or similar.
        WASMType *target_core_wasm_pseudo_type = NULL; // Placeholder
        // This should be derived from core_func_wasm_type->types[current_core_argv_idx] if it's a 1:1 map
        // or from get_component_type_core_abi_details if complex.
        // For now, assume lower_value can work with the component type and a basic hint.
        uint8 core_type_tag_for_lower = VALUE_TYPE_I32; // Default for refs

        if (param_comp_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
            bool success_map_prim;
            core_type_tag_for_lower = get_core_wasm_type_for_primitive(param_comp_type->u.primitive, &success_map_prim);
            if (!success_map_prim) {
                LOG_ERROR("GenericLiftedThunk: Unsupported primitive type for lowering param %u.", i);
                bh_free(core_argv); return false;
            }
        }

        LOG_VERBOSE("Lowering arg %i, host_arg_val_ptr %p, comp_type kind %d, core_type_hint %d",
                    i, host_arg_val_ptr, param_comp_type->kind, core_type_tag_for_lower);

        if (!wasm_component_canon_lower_value(
                target_exec_env, // Exec env of the target core module for memory operations
                thunk_ctx->canonical_def,
                NULL, // host_component_value for direct value, NULL if using ptr
                host_arg_val_ptr, // Direct pointer to host data (e.g. HostComponentList*, WAMRHostGeneralValue*)
                param_comp_type,
                core_type_tag_for_lower, // Hint: actual core types are determined by ABI rules in lower_value
                &core_argv[current_core_argv_idx],
                error_buf_internal, sizeof(error_buf_internal)
            )) {
            LOG_ERROR("GenericLiftedThunk: Failed to lower argument %u. Error: %s", i, error_buf_internal);
            bh_free(core_argv);
            // wasm_runtime_set_exception with error_buf_internal
            return false;
        }
        // Advance current_core_argv_idx based on the actual number of core cells consumed.
        // wasm_component_canon_lower_value should ideally return this.
        // For now, assume it's mostly 1 cell (I32 offset or direct primitive).
        // This needs to be accurate based on get_component_type_core_abi_details.
        // For now, using the hint's cell size.
        current_core_argv_idx += wasm_value_type_cell_num(core_type_tag_for_lower);
        if (current_core_argv_idx > core_param_cell_num) { // Check against total expected cells
            LOG_ERROR("GenericLiftedThunk: Core argument index overflow during lowering.");
            bh_free(core_argv); return false;
        }
    }

    // Call the target core Wasm function
    if (!wasm_runtime_call_wasm(target_exec_env, core_func_resolved, core_param_cell_num, core_argv)) {
        // Exception should be set by wasm_runtime_call_wasm
        LOG_ERROR("GenericLiftedThunk: Core Wasm function call failed. Exception: %s", wasm_runtime_get_exception(thunk_ctx->target_core_module_inst));
        bh_free(core_argv);
        return false; // Propagate failure
    }

    // 2. Result Lifting Loop
    // Results are in core_argv, starting after param cells.
    uint32 current_core_result_argv_idx = core_param_cell_num;
    if (comp_func_type->result) { // Assuming single direct result or result-as-tuple
        WASMComponentValType *result_comp_type = comp_func_type->result;
        void **host_result_ptr_target = (void **)((uintptr_t)host_argv_raw[0 + 1 + num_comp_params]); // Result ptr is after context and args
                                                                                                    // This assumes host_argv_raw is large enough for [context, args..., results_ptr...]

        // Determine source_core_wasm_type for this result.
        // Similar to lowering, this is complex.
        uint8 core_type_tag_for_lift = VALUE_TYPE_I32; // Default for refs
        if (result_comp_type->kind == VAL_TYPE_KIND_PRIMITIVE) {
            bool success_map_prim;
            core_type_tag_for_lift = get_core_wasm_type_for_primitive(result_comp_type->u.primitive, &success_map_prim);
            if (!success_map_prim) {
                LOG_ERROR("GenericLiftedThunk: Unsupported primitive type for lifting result.");
                bh_free(core_argv); return false;
            }
        }

        LOG_VERBOSE("Lifting result, comp_type kind %d, core_type_hint %d, core_argv_offset %u",
                    result_comp_type->kind, core_type_tag_for_lift, current_core_result_argv_idx);

        if (!wasm_component_canon_lift_value(
                target_exec_env, // Exec env of the target core module
                thunk_ctx->canonical_def,
                &core_argv[current_core_result_argv_idx],
                core_type_tag_for_lift, // Hint for the core type(s) at core_argv
                result_comp_type,
                host_result_ptr_target, // Target for the pointer to lifted host value
                error_buf_internal, sizeof(error_buf_internal)
            )) {
            LOG_ERROR("GenericLiftedThunk: Failed to lift result. Error: %s", error_buf_internal);
            bh_free(core_argv);
            // wasm_runtime_set_exception
            return false;
        }
        // Advance current_core_result_argv_idx if multiple results were possible,
        // or if a single result spanned multiple cells.
        // current_core_result_argv_idx += wasm_value_type_cell_num(core_type_tag_for_lift);
    } else { // No component-level result
        if (core_result_cell_num > 0) {
            LOG_WARNING("GenericLiftedThunk: Core function returned %u cells, but component function has no result. Discarding core results.", core_result_cell_num);
        }
    }

    // 3. Post-Return Cleanup
    uint32 post_return_func_idx = (uint32)-1;
    for (uint32 k=0; k < thunk_ctx->canonical_def->option_count; ++k) {
        if (thunk_ctx->canonical_def->options[k].kind == CANONICAL_OPTION_POST_RETURN_FUNC_IDX) {
            post_return_func_idx = thunk_ctx->canonical_def->options[k].value;
            break;
        }
    }

    if (post_return_func_idx != (uint32)-1) {
        WASMFunctionInstance *post_return_func = wasm_get_function(thunk_ctx->target_core_module_inst, post_return_func_idx);
        if (!post_return_func) {
            LOG_ERROR("GenericLiftedThunk: Post-return function index %u not found in target core module.", post_return_func_idx);
            // This might be a critical error, depending on policy.
        } else {
            LOG_VERBOSE("Executing post-return function (core func idx %u).", post_return_func_idx);
            if (!wasm_runtime_call_wasm(target_exec_env, post_return_func, 0, NULL)) { // post-return is func()
                LOG_ERROR("GenericLiftedThunk: Post-return function call failed. Exception: %s", wasm_runtime_get_exception(thunk_ctx->target_core_module_inst));
                // Handle or propagate exception if needed.
            }
        }
    } else {
        LOG_TODO("GenericLiftedThunk: No post-return function. Implement robust memory cleanup for lowered arguments if needed (e.g., for by-value strings/lists allocated by lower_value).");
        // This is where one might call wasm_runtime_module_free for memory allocated by wasm_component_canon_lower_value
        // if lower_value returned specific offsets and a mechanism exists to track them.
        // For now, relying on post-return or guest-managed memory.
    }


    if (core_argv) {
        bh_free(core_argv);
    }
    return true; // Indicate success
}


static bool execute_component_start_function(WASMComponentInstanceInternal *comp_inst, 
                                             WASMComponentStart *start_def, 
                                             char *error_buf, uint32 error_buf_size);

// Forward declaration for resolve_component_alias_by_index (if needed, or define before use)
// static bool
// resolve_component_alias_by_index(WASMComponentInstanceInternal *comp_inst,
//                                  uint32 alias_idx,
//                                  WASMAliasSort expected_sort,
//                                  ResolvedComponentItem *out_resolved_item,
//                                  char *error_buf, uint32 error_buf_size);


static bool
resolve_component_alias_by_index(WASMComponentInstanceInternal *comp_inst,
                                 uint32 alias_idx,
                                 WASMAliasSort expected_sort,
                                 ResolvedComponentItem *out_resolved_item,
                                 char *error_buf, uint32 error_buf_size)
{
    WASMComponent *component_def = comp_inst->component_def;
    WASMComponentAlias *alias_def;

    if (!out_resolved_item) {
        set_comp_rt_error(error_buf, error_buf_size, "resolve_component_alias_by_index: out_resolved_item is NULL.");
        return false;
    }
    memset(out_resolved_item, 0, sizeof(ResolvedComponentItem));

    if (!component_def) {
        set_comp_rt_error(error_buf, error_buf_size, "Component definition is NULL in component instance.");
        return false;
    }

    if (alias_idx >= component_def->alias_count) {
        set_comp_rt_error_v(error_buf, error_buf_size, "Alias index %u out of bounds (count %u).",
                           alias_idx, component_def->alias_count);
        return false;
    }

    alias_def = &component_def->aliases[alias_idx];

    if (alias_def->sort != expected_sort) {
        // This check might be too strict if the caller doesn't know the exact sort
        // or if sorts are compatible (e.g. aliasing a core func as a component func via lift).
        LOG_WARNING("Alias sort mismatch: requested %d, alias definition sort %d for alias_idx %u.",
                    expected_sort, alias_def->sort, alias_idx);
        // Allow proceeding for now, the user of the resolved item should validate the final kind.
    }

    LOG_VERBOSE("Resolving alias_idx %u: sort %u, target_kind %u, target_idx %u, target_name '%s'",
                alias_idx, alias_def->sort, alias_def->target_kind, alias_def->target_idx,
                alias_def->target_name ? alias_def->target_name : "N/A");

    switch (alias_def->target_kind) {
        case ALIAS_TARGET_CORE_EXPORT:
        {
            if (alias_def->target_idx >= component_def->core_instance_count) {
                set_comp_rt_error_v(error_buf, error_buf_size, "Alias target_idx %u for core export is out of bounds for core_instances (count %u).",
                                   alias_def->target_idx, component_def->core_instance_count);
                return false;
            }

            WASMComponentCoreInstance *core_inst_def = &component_def->core_instances[alias_def->target_idx];
            if (core_inst_def->kind != CORE_INSTANCE_KIND_INSTANTIATE) {
                set_comp_rt_error_v(error_buf, error_buf_size, "Alias target_idx %u for core export points to a non-instantiated core instance definition (kind %u).",
                                   alias_def->target_idx, core_inst_def->kind);
                return false;
            }

            if (!comp_inst->core_instance_map) {
                set_comp_rt_error(error_buf, error_buf_size, "Core instance map is NULL in component instance.");
                return false;
            }
            uint32 runtime_module_array_idx = comp_inst->core_instance_map[alias_def->target_idx];
            if (runtime_module_array_idx == (uint32)-1 || runtime_module_array_idx >= comp_inst->num_module_instances) {
                 set_comp_rt_error_v(error_buf, error_buf_size, "Alias target_idx %u for core export: cannot map to valid runtime core module instance (map_val %u, num_mods %u).",
                                    alias_def->target_idx, runtime_module_array_idx, comp_inst->num_module_instances);
                 return false;
            }
            WASMModuleInstance *target_core_mod_inst = comp_inst->module_instances[runtime_module_array_idx];
            if (!target_core_mod_inst) {
                 set_comp_rt_error_v(error_buf, error_buf_size, "Alias target_idx %u for core export: runtime core module instance is NULL.", alias_def->target_idx);
                 return false;
            }

            if (!alias_def->target_name) {
                set_comp_rt_error(error_buf, error_buf_size, "Alias target_name for core export is NULL.");
                return false;
            }

            out_resolved_item->item.core_item.core_module_inst = target_core_mod_inst;
            out_resolved_item->item.core_item.item_idx_in_module = (uint32)-1;

            switch (alias_def->sort) { // expected_sort might be more general, alias_def->sort is specific
                case ALIAS_SORT_CORE_FUNC:
                {
                    WASMFunctionInstance *func_inst = find_exported_function_instance(target_core_mod_inst, alias_def->target_name, error_buf, error_buf_size);
                    if (!func_inst) return false; // Error set by find_exported_function_instance

                    out_resolved_item->kind = RESOLVED_ITEM_CORE_FUNC;
                    out_resolved_item->item.core_item.core_item_kind = WASM_EXTERNAL_FUNCTION;
                    // Iterate exports to find the index of the function in the module's function space
                    bool found_idx = false;
                    for(uint32 exp_i=0; exp_i < target_core_mod_inst->export_func_count; ++exp_i) {
                        if (target_core_mod_inst->export_functions[exp_i].function == func_inst) {
                            out_resolved_item->item.core_item.item_idx_in_module = target_core_mod_inst->export_functions[exp_i].func_idx_in_module;
                            found_idx = true;
                            break;
                        }
                    }
                    if (!found_idx) {
                        // This case should ideally not happen if find_exported_function_instance succeeded from exports.
                        // It implies func_inst was found but not via iterating module's own export_functions list,
                        // which is how find_exported_function_instance works.
                        LOG_WARNING("Could not determine func_idx_in_module for core func export '%s' though function was found.", alias_def->target_name);
                    }
                    break;
                }
                // TODO: Cases for ALIAS_SORT_CORE_TABLE, ALIAS_SORT_CORE_MEMORY, ALIAS_SORT_CORE_GLOBAL
                default:
                    set_comp_rt_error_v(error_buf, error_buf_size, "Unsupported alias sort %d for ALIAS_TARGET_CORE_EXPORT.", alias_def->sort);
                    return false;
            }
            break;
        }
        case ALIAS_TARGET_OUTER:
            // TODO: Implement outer alias resolution. Requires parent pointer and recursive call.
            // WASMComponentInstanceInternal *parent_inst = comp_inst->parent_comp_inst;
            // if (!parent_inst) {
            //     set_comp_rt_error(error_buf, error_buf_size, "ALIAS_TARGET_OUTER: No parent component instance.");
            //     return false;
            // }
            // if (alias_def->target_outer_depth == 0) { /* Should be at least 1 for outer */ }
            // ... traverse alias_def->target_outer_depth levels ...
            // Then resolve alias_def->target_idx in that parent/ancestor.
            set_comp_rt_error(error_buf, error_buf_size, "ALIAS_TARGET_OUTER not yet implemented.");
            return false;

        case ALIAS_TARGET_CORE_MODULE:
            if (alias_def->target_idx >= component_def->core_module_count) { // core_module_count is count of *definitions*
                 set_comp_rt_error_v(error_buf, error_buf_size, "Alias target_idx %u for CORE_MODULE out of bounds (core_module_count %u).",
                                    alias_def->target_idx, component_def->core_module_count);
                 return false;
            }
            // This alias refers to a core module *definition*. To make it resolvable to a runtime instance,
            // we need to find which *core instance definition* instantiates this module definition.
            // This mapping is not direct. A module definition can be instantiated multiple times or not at all.
            // The current structure of ALIAS_TARGET_CORE_MODULE seems to imply `target_idx` is a core_module_def_idx.
            // Let's assume it means "the first runtime instance of this core module definition".
            // This is ambiguous and needs clarification from Component Model spec usage.
            // For now, try to find a core_instance_def that instantiates component_def->core_modules[alias_def->target_idx]
            // and then map that core_instance_def to a runtime instance.
            uint32 found_core_instance_def_idx = (uint32)-1;
            for (uint32 cid_idx = 0; cid_idx < component_def->core_instance_count; ++cid_idx) {
                if (component_def->core_instances[cid_idx].kind == CORE_INSTANCE_KIND_INSTANTIATE &&
                    component_def->core_instances[cid_idx].u.instantiate.module_idx == alias_def->target_idx) {
                    found_core_instance_def_idx = cid_idx;
                    break;
                }
            }
            if (found_core_instance_def_idx == (uint32)-1) {
                set_comp_rt_error_v(error_buf, error_buf_size, "Alias for CORE_MODULE target_def_idx %u: no core instance found that instantiates this module definition.", alias_def->target_idx);
                return false;
            }
            if (!comp_inst->core_instance_map) {
                set_comp_rt_error(error_buf, error_buf_size, "Core instance map is NULL (resolving ALIAS_TARGET_CORE_MODULE).");
                return false;
            }
            uint32 runtime_mod_idx = comp_inst->core_instance_map[found_core_instance_def_idx];
            if (runtime_mod_idx == (uint32)-1 || runtime_mod_idx >= comp_inst->num_module_instances) {
                 set_comp_rt_error_v(error_buf, error_buf_size, "Alias for CORE_MODULE (def_idx %u, core_inst_def_idx %u): cannot map to valid runtime core module instance (map_val %u).",
                                    alias_def->target_idx, found_core_instance_def_idx, runtime_mod_idx);
                 return false;
            }
            out_resolved_item->kind = RESOLVED_ITEM_MODULE;
            out_resolved_item->item.core_module_inst_ptr = comp_inst->module_instances[runtime_mod_idx];
            break;

        case ALIAS_TARGET_TYPE:
            if (alias_def->target_idx >= component_def->type_definition_count) {
                 set_comp_rt_error_v(error_buf, error_buf_size, "Alias target_idx %u for TYPE out of bounds (type_definition_count %u).",
                                    alias_def->target_idx, component_def->type_definition_count);
                 return false;
            }
            out_resolved_item->kind = RESOLVED_ITEM_TYPE;
            out_resolved_item->item.type_def_ptr = &component_def->type_definitions[alias_def->target_idx];
            break;

        case ALIAS_TARGET_COMPONENT: // Refers to a nested component *definition*
            if (alias_def->target_idx >= component_def->nested_component_count) {
                 set_comp_rt_error_v(error_buf, error_buf_size, "Alias target_idx %u for COMPONENT out of bounds (nested_component_count %u).",
                                    alias_def->target_idx, component_def->nested_component_count);
                 return false;
            }
            // This resolves to the WASMComponent* definition.
            // A runtime scenario would typically want an *instance* of this component.
            // This suggests ALIAS_TARGET_COMPONENT is for type-level operations or re-exporting definitions.
            out_resolved_item->kind = RESOLVED_ITEM_NONE; // Or a new kind RESOLVED_ITEM_COMPONENT_DEFINITION
            out_resolved_item->item.ptr = component_def->nested_components[alias_def->target_idx].parsed_component;
            if (!out_resolved_item->item.ptr) { // Fallback if not parsed (should be parsed by instantiation time)
                 out_resolved_item->item.ptr = (void*)component_def->nested_components[alias_def->target_idx].component_data;
                 LOG_WARNING("Alias to COMPONENT definition (idx %u) resolved to raw data as parsed_component is NULL.", alias_def->target_idx);
            } else {
                 LOG_VERBOSE("Alias to COMPONENT definition (idx %u) resolved to parsed_component.", alias_def->target_idx);
            }
            break;

        case ALIAS_TARGET_INSTANCE:
            // This refers to an *instance definition* (WASMComponentCompInstance) within the current component.
            // alias_def->target_idx is an index into component_def->component_instances.
            if (alias_def->target_idx >= component_def->component_instance_count) {
                 set_comp_rt_error_v(error_buf, error_buf_size, "Alias target_idx %u for INSTANCE out of bounds (component_instance_count %u).",
                                    alias_def->target_idx, component_def->component_instance_count);
                 return false;
            }
            // This instance definition could be for a core module or a nested component.
            // This needs mapping from this definition-time index to a runtime instance.
            // This is complex as it requires knowing which runtime instance (module_instances or component_instances)
            // corresponds to `component_def->component_instances[alias_def->target_idx]`.
            // This mapping is not straightforward with current structures.
            // TODO: Need a robust way to map WASMComponentCompInstance definition to its runtime counterpart.
            LOG_TODO("Alias resolution for ALIAS_TARGET_INSTANCE: mapping definition to runtime instance is complex and not fully implemented.");
            out_resolved_item->kind = RESOLVED_ITEM_NONE; // Placeholder
            set_comp_rt_error(error_buf, error_buf_size, "ALIAS_TARGET_INSTANCE resolution logic incomplete (mapping def to runtime).");
            return false;

        default:
            set_comp_rt_error_v(error_buf, error_buf_size, "Unknown alias target kind: %u.", alias_def->target_kind);
            return false;
    }

    return true;
}


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

                LOG_VERBOSE("Export '%s': Attempting to resolve core_func_idx %u from canonical_def as an alias.",
                            export_def->name, canonical_def->u.lift.core_func_idx);

                ResolvedComponentItem resolved_core_func_item;
                // Assumption: canonical_def->u.lift.core_func_idx is an index into the component's alias table.
                // The alias is expected to resolve to a core function.
                if (!resolve_component_alias_by_index(comp_inst,
                                                      canonical_def->u.lift.core_func_idx,
                                                      ALIAS_SORT_CORE_FUNC, // Expected sort of the alias itself
                                                      &resolved_core_func_item,
                                                      error_buf, error_buf_size)) {
                    // resolve_component_alias_by_index sets the error_buf.
                    LOG_ERROR("Export '%s': Failed to resolve core_func_idx %u (alias_idx) via alias mechanism: %s",
                               export_def->name, canonical_def->u.lift.core_func_idx, error_buf);
                    resolved_export->item.function_thunk_context = NULL; // Mark as failed
                    break; // Break from this export case, try next export_def
                }

                if (resolved_core_func_item.kind != RESOLVED_ITEM_CORE_FUNC) {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Expected resolved core_func_idx %u (alias_idx) to be RESOLVED_ITEM_CORE_FUNC, but got kind %d.",
                                       export_def->name, canonical_def->u.lift.core_func_idx, resolved_core_func_item.kind);
                    resolved_export->item.function_thunk_context = NULL; // Mark as failed
                    // Log the error, but don't fail the entire export population yet.
                    // The error_buf will be checked by the caller of populate_exports.
                    LOG_ERROR("Export '%s': Resolution of core_func_idx %u resulted in unexpected kind %d.",
                              export_def->name, canonical_def->u.lift.core_func_idx, resolved_core_func_item.kind);
                    break; // Break from this export case
                }

                WASMModuleInstance* target_core_inst = resolved_core_func_item.item.core_item.core_module_inst;
                uint32 core_func_idx_in_mod = resolved_core_func_item.item.core_item.item_idx_in_module;

                if (!target_core_inst || core_func_idx_in_mod == (uint32)-1) {
                    // (uint32)-1 is a common sentinel for "not found" or "invalid".
                    // If item_idx_in_module is truly (uint32)-1 from a successful resolution,
                    // it means the function was found but its index couldn't be determined, which is an issue.
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Resolved core function item has NULL instance or invalid function index in module (%u).",
                                       export_def->name, core_func_idx_in_mod);
                    LOG_ERROR("Export '%s': Resolved core function item from alias %u has NULL instance or invalid function index in module (%u). Target core inst: %p",
                              export_def->name, canonical_def->u.lift.core_func_idx, core_func_idx_in_mod, target_core_inst);
                    resolved_export->item.function_thunk_context = NULL;
                    break; // Break from this export case
                }

                LOG_VERBOSE("Export '%s': Resolved core_func_idx %u (alias) to target_core_inst %p, func_idx_in_mod %u.",
                            export_def->name, canonical_def->u.lift.core_func_idx,
                            (void*)target_core_inst, core_func_idx_in_mod);

                // Create the thunk with the resolved information
                // comp_inst->exec_env is the exec_env of the current component instance.
                // For lifted functions, the thunk runs in this component's context.
                resolved_export->item.function_thunk_context = create_lifted_function_thunk(
                    comp_inst->exec_env, /* The exec_env of the component instance being populated. */
                    comp_inst,           /* Pass the component instance that will own this thunk. */
                    canonical_def,
                    target_core_inst,
                    core_func_idx_in_mod,
                    comp_func_type,
                    error_buf, error_buf_size);

                if (!resolved_export->item.function_thunk_context) {
                    // error_buf should be set by create_lifted_function_thunk
                    if (strstr(error_buf, "placeholder") == NULL && strstr(error_buf, "not yet implemented") == NULL
                        && strstr(error_buf, "Failed to allocate LiftedFuncThunkContext") == NULL /* Allow this specific alloc error to not fail all exports */) {
                         LOG_ERROR("Export '%s': Failed to create lifted function thunk: %s", export_def->name, error_buf);
                         // This is a more critical failure if thunk creation fails for non-placeholder reasons.
                         // Consider goto fail_export if this happens. For now, matching prompt's softer error handling.
                         // goto fail_export;
                    } else {
                         LOG_WARNING("Export '%s': Lifted function thunk creation is a placeholder, not implemented, or failed allocation: %s", export_def->name, error_buf);
                         // Set to NULL so it's known it's not callable, but don't fail all exports.
                         resolved_export->item.function_thunk_context = NULL;
                    }
                }
                // TODO: Type validation against export_def->optional_desc_type_idx
                break;
            }
            case EXPORT_KIND_INSTANCE:
            case EXPORT_KIND_COMPONENT: // Treat component export similar to instance export for now
            {
                WASMAliasSort sort_for_alias = (export_def->kind == EXPORT_KIND_INSTANCE) ? ALIAS_SORT_INSTANCE : ALIAS_SORT_COMPONENT;
                ResolvedComponentItem resolved_item;
                LOG_VERBOSE("Export '%s': Attempting to resolve item_idx %u as alias (sort %d).",
                            export_def->name, export_def->item_idx, sort_for_alias);

                // item_idx from export_def is treated as a potential alias_idx
                if (!resolve_component_alias_by_index(comp_inst, export_def->item_idx,
                                                      sort_for_alias,
                                                      &resolved_item,
                                                      error_buf, error_buf_size)) {
                    LOG_ERROR("Export '%s': Failed to resolve item_idx %u via alias: %s. Marking export as unavailable.",
                               export_def->name, export_def->item_idx, error_buf);
                    // Clear any potentially partially set error by resolve_component_alias_by_index if we are not failing all exports
                    // error_buf[0] = '\0';
                    resolved_export->item.instance_ptr = NULL; // Or component_ptr, mark as unresolved
                    break;
                }

                if (export_def->kind == EXPORT_KIND_INSTANCE && resolved_item.kind != RESOLVED_ITEM_INSTANCE && resolved_item.kind != RESOLVED_ITEM_MODULE) {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Expected resolved item for INSTANCE to be INSTANCE or MODULE, got %d.",
                                       export_def->name, resolved_item.kind);
                    LOG_ERROR("Export '%s': Expected INSTANCE or MODULE, got %d. Marking export as unavailable.", export_def->name, resolved_item.kind);
                    resolved_export->item.instance_ptr = NULL;
                    break;
                }
                if (export_def->kind == EXPORT_KIND_COMPONENT && resolved_item.kind != RESOLVED_ITEM_COMPONENT) {
                     set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Expected resolved item for COMPONENT to be COMPONENT, got %d.",
                                       export_def->name, resolved_item.kind);
                     LOG_ERROR("Export '%s': Expected COMPONENT, got %d. Marking export as unavailable.", export_def->name, resolved_item.kind);
                     resolved_export->item.component_ptr = NULL;
                     break;
                }

                // Store the resolved item.
                if (resolved_item.kind == RESOLVED_ITEM_INSTANCE) {
                    resolved_export->item.instance_ptr = resolved_item.item.comp_inst_ptr;
                } else if (resolved_item.kind == RESOLVED_ITEM_MODULE) {
                    // Assuming ResolvedComponentExportItem.item.instance_ptr can store WASMModuleInstance* after cast
                    // This implies that consumers of this export need to be aware of the possibility.
                    // Alternatively, ResolvedComponentExportItem might need separate fields or a sub-union.
                    resolved_export->item.instance_ptr = (WASMComponentInstanceInternal*)resolved_item.item.core_module_inst_ptr;
                    LOG_VERBOSE("Export '%s': Storing resolved core_module_inst %p into instance_ptr via cast.", export_def->name, (void*)resolved_item.item.core_module_inst_ptr);
                } else if (resolved_item.kind == RESOLVED_ITEM_COMPONENT) {
                    resolved_export->item.component_ptr = resolved_item.item.comp_inst_ptr;
                }
                // TODO: Type validation against export_def->optional_desc_type_idx (should be an instance or component type)
                // This requires resolving optional_desc_type_idx to a WASMComponentDefinedType and then comparing.
                LOG_VERBOSE("Export '%s': Successfully resolved item_idx %u to runtime item kind %d.",
                            export_def->name, export_def->item_idx, resolved_item.kind);
                break;
            }
            case EXPORT_KIND_TYPE:
            {
                ResolvedComponentItem resolved_item;
                LOG_VERBOSE("Export '%s': Attempting to resolve item_idx %u as alias (sort ALIAS_SORT_TYPE).",
                            export_def->name, export_def->item_idx);
                // item_idx from export_def is treated as a potential alias_idx
                if (!resolve_component_alias_by_index(comp_inst, export_def->item_idx,
                                                      ALIAS_SORT_TYPE,
                                                      &resolved_item,
                                                      error_buf, error_buf_size)) {
                    LOG_ERROR("Export '%s': Failed to resolve item_idx %u for TYPE via alias: %s. Marking export as unavailable.",
                               export_def->name, export_def->item_idx, error_buf);
                    resolved_export->item.type_definition = NULL;
                    break;
                }
                if (resolved_item.kind != RESOLVED_ITEM_TYPE) {
                     set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Expected resolved item for TYPE to be TYPE, got %d.",
                                       export_def->name, resolved_item.kind);
                     LOG_ERROR("Export '%s': Expected TYPE, got %d. Marking export as unavailable.", export_def->name, resolved_item.kind);
                     resolved_export->item.type_definition = NULL;
                     break;
                }
                resolved_export->item.type_definition = resolved_item.item.type_def_ptr;
                LOG_VERBOSE("Export '%s': Successfully resolved type item_idx %u.", export_def->name, export_def->item_idx);
                break;
            }
            case EXPORT_KIND_VALUE:
            {
                // Placeholder for ALIAS_SORT_VALUE, assuming it might be 0x04 based on typical sort order,
                // but this needs to be defined in wasm_component_loader.h's WASMAliasSort.
                // Using a temporary value that indicates "undefined" for now.
                WASMAliasSort sort_for_alias_value = (WASMAliasSort)0xFFFFFFFF;
                const char* alias_sort_value_str = "ALIAS_SORT_VALUE"; // For logging

                // Attempt to map EXPORT_KIND_VALUE to a conceptual ALIAS_SORT_VALUE
                // This mapping should ideally be defined in wasm_component_loader.h or common header
                // For now, we simulate this. If EXPORT_KIND_VALUE is, say, 4, and ALIAS_SORT_VALUE is also 4.
                // Let's assume such a mapping exists and ALIAS_SORT_VALUE is a valid enum member.
                // To make this runnable, we need a placeholder. If actual ALIAS_SORT_VALUE is known, use it.
                // For this exercise, let's assume ALIAS_SORT_VALUE might be, e.g. 5, if others are 0-4.
                // This is speculative. The actual value must come from the enum definition.
                // If no direct ALIAS_SORT_VALUE, then value exports cannot go through this alias path.

                // Find ALIAS_SORT_VALUE definition or use placeholder
                // #if defined(ALIAS_SORT_VALUE)
                // sort_for_alias_value = ALIAS_SORT_VALUE;
                // #else
                // LOG_WARNING("Export '%s': ALIAS_SORT_VALUE is not defined. Value export via alias is effectively disabled.", export_def->name);
                // #endif
                // Given I can't check header, I'll proceed with the placeholder and conditional logic.

                LOG_TODO("Export '%s': Value export resolution needs a defined ALIAS_SORT_VALUE and robust value item resolution logic.", export_def->name);
                resolved_export->item.value_ptr = NULL; // Ensure it's NULL if not resolved.

                if (sort_for_alias_value != (WASMAliasSort)0xFFFFFFFF) {
                    ResolvedComponentItem resolved_item;
                    LOG_VERBOSE("Export '%s': Attempting to resolve item_idx %u as alias (sort %s).",
                                export_def->name, export_def->item_idx, alias_sort_value_str);
                    if (!resolve_component_alias_by_index(comp_inst, export_def->item_idx,
                                                          sort_for_alias_value,
                                                          &resolved_item,
                                                          error_buf, error_buf_size)) {
                        LOG_ERROR("Export '%s': Failed to resolve item_idx %u for VALUE via alias: %s. Marking as unavailable.",
                                   export_def->name, export_def->item_idx, error_buf);
                        break;
                    }
                    if (resolved_item.kind != RESOLVED_ITEM_VALUE) {
                         set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Expected resolved item for VALUE to be VALUE, got %d.",
                                           export_def->name, resolved_item.kind);
                         LOG_ERROR("Export '%s': Expected VALUE, got %d. Marking as unavailable.", export_def->name, resolved_item.kind);
                         break;
                    }
                    // resolved_export->item.value_ptr = resolved_item.item.value_ptr; // If value_ptr exists in ResolvedComponentItem
                    LOG_VERBOSE("Export '%s': Successfully resolved value item_idx %u (placeholder).", export_def->name, export_def->item_idx);
                } else {
                    // If ALIAS_SORT_VALUE is not defined/known, cannot use resolve_component_alias_by_index.
                    // Fallback to direct resolution if that's ever supported for values, or mark as error.
                    // For now, consistent with prompt, this means it's an error if it was meant to be an alias.
                    // If item_idx was a direct index to a value, that's a different logic path not covered by this subtask.
                    set_comp_rt_error_v(error_buf, error_buf_size, "Export '%s': Value export resolution skipped as ALIAS_SORT_VALUE is undefined/unknown.", export_def->name);
                    LOG_ERROR("Export '%s': Value export item_idx %u cannot be resolved as ALIAS_SORT_VALUE is not available.", export_def->name, export_def->item_idx);
                }
                break;
            }
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

    // 2. Resolve Arguments and Prepare for Thunk Call:
    // The generic_lifted_thunk_executor expects argv[0] to be the context,
    // and subsequent elements to be pointers to argument data.
    uint32 total_thunk_argc = 1 + start_def->arg_count; // Context + actual args
    uint32 *host_argv_for_thunk = NULL; // Array of uint32/uintptr_t for thunk

    if (total_thunk_argc > 0) { // Always true since context is first arg
        host_argv_for_thunk = bh_malloc(total_thunk_argc * sizeof(uint32));
        if (!host_argv_for_thunk) {
            set_comp_rt_error(error_buf, error_buf_size, "Failed to allocate memory for start function thunk arguments.");
            return false;
        }
        memset(host_argv_for_thunk, 0, total_thunk_argc * sizeof(uint32));
        host_argv_for_thunk[0] = (uint32)(uintptr_t)thunk_ctx;
    } else { // Should not happen
        set_comp_rt_error(error_buf, error_buf_size, "Internal error: total_thunk_argc is 0 for start function.");
        return false;
    }


    if (start_def->arg_count > 0) {
        if (!component_def->values || component_def->value_count == 0) {
            set_comp_rt_error(error_buf, error_buf_size, "Start function requires arguments, but component has no defined values.");
            bh_free(host_argv_for_thunk);
            return false;
        }

        for (uint32 j = 0; j < start_def->arg_count; ++j) {
            uint32 value_idx = start_def->arg_value_indices[j];
            if (value_idx >= component_def->value_count) {
                set_comp_rt_error_v(error_buf, error_buf_size, "Start function arg %u: value index %u out of bounds (count %u).",
                                   j, value_idx, component_def->value_count);
                bh_free(host_argv_for_thunk);
                return false;
            }
            WASMComponentValue *val_def_literal = &component_def->values[value_idx];

            if (j >= func_type->param_count) {
                 set_comp_rt_error_v(error_buf, error_buf_size, "Start function arg %u: exceeds expected parameter count %u.",
                                   j, func_type->param_count);
                bh_free(host_argv_for_thunk);
                return false;
            }
            WASMComponentValType *expected_param_type = func_type->params[j].valtype;

            // Perform type checking (simplified, full type equivalence is complex)
            // TODO: Enhance this type check to be more comprehensive, e.g., using a helper function.
            if (val_def_literal->parsed_type.kind != expected_param_type->kind) {
                 LOG_ERROR("Start function arg %u: kind mismatch. Value has kind %d, expected kind %d.",
                          j, val_def_literal->parsed_type.kind, expected_param_type->kind);
                set_comp_rt_error_v(error_buf, error_buf_size, "Start function arg %u: type kind mismatch with defined value.", j);
                bh_free(host_argv_for_thunk);
                return false;
            }
            if (val_def_literal->parsed_type.kind == VAL_TYPE_KIND_PRIMITIVE &&
                val_def_literal->parsed_type.u.primitive != expected_param_type->u.primitive) {
                 LOG_ERROR("Start function arg %u: primitive type mismatch. Value primitive %d, expected primitive %d.",
                          j, val_def_literal->parsed_type.u.primitive, expected_param_type->u.primitive);
                set_comp_rt_error_v(error_buf, error_buf_size, "Start function arg %u: primitive type mismatch.", j);
                bh_free(host_argv_for_thunk);
                return false;
            }
            // For composite types, a deeper check involving recursive comparison of structure would be needed here.
            // For now, only kind and primitive subtype are checked.

            void *ptr_to_arg_data = NULL;
            if (val_def_literal->parsed_type.kind == VAL_TYPE_KIND_PRIMITIVE) {
                switch (val_def_literal->parsed_type.u.primitive) {
                    case PRIM_VAL_BOOL: ptr_to_arg_data = &val_def_literal->val.b; break;
                    case PRIM_VAL_S8:   ptr_to_arg_data = &val_def_literal->val.s8; break;
                    case PRIM_VAL_U8:   ptr_to_arg_data = &val_def_literal->val.u8; break;
                    case PRIM_VAL_S16:  ptr_to_arg_data = &val_def_literal->val.s16; break;
                    case PRIM_VAL_U16:  ptr_to_arg_data = &val_def_literal->val.u16; break;
                    case PRIM_VAL_S32:  ptr_to_arg_data = &val_def_literal->val.s32; break;
                    case PRIM_VAL_U32:  ptr_to_arg_data = &val_def_literal->val.u32; break;
                    case PRIM_VAL_S64:  ptr_to_arg_data = &val_def_literal->val.s64; break;
                    case PRIM_VAL_U64:  ptr_to_arg_data = &val_def_literal->val.u64; break;
                    case PRIM_VAL_F32:  ptr_to_arg_data = &val_def_literal->val.f32; break;
                    case PRIM_VAL_F64:  ptr_to_arg_data = &val_def_literal->val.f64; break;
                    case PRIM_VAL_CHAR: ptr_to_arg_data = &val_def_literal->val.u32; break; // Char stored as u32
                    case PRIM_VAL_STRING:
                        // For string, wasm_component_canon_lower_value expects char** if it's a string to be copied.
                        // If it's handling a direct char*, it needs to be clear.
                        // The host_arg_val_ptr in generic_lifted_thunk_executor is void*.
                        // If lower_value for string expects char*, then pass string_val directly.
                        // If it expects char**, then pass &string_val.
                        // Current generic_lifted_thunk_executor passes host_arg_val_ptr directly to lower_value.
                        // Let's assume lower_value handles char* for string input from component values.
                        ptr_to_arg_data = (void*)val_def_literal->val.string_val;
                        // LOG_TODO: Clarify if wasm_component_canon_lower_value for string takes char* or char**
                        // when the source is a component value literal. Assuming char* for now.
                        break;
                    default:
                        set_comp_rt_error_v(error_buf, error_buf_size, "Start function arg %u: unhandled primitive type %d.",
                                           j, val_def_literal->parsed_type.u.primitive);
                        bh_free(host_argv_for_thunk);
                        return false;
                }
            } else { // Composite types (List, Record, etc.)
                // val_def_literal->val.ptr_val points to the host-allocated literal structure
                // (e.g., WASMComponentValue_list_literal*).
                // wasm_component_canon_lower_value needs to be able to handle these specific structures.
                ptr_to_arg_data = val_def_literal->val.ptr_val;
                LOG_TODO("Ensure wasm_component_canon_lower_value can handle WASMComponentValue_xxx_literal structs for start function args.");
            }
            host_argv_for_thunk[j + 1] = (uint32)(uintptr_t)ptr_to_arg_data;
        }
        LOG_VERBOSE("Resolved %u arguments for start function '%s'.", start_def->arg_count, target_export_func->name);
    }

    // 3. Invoke Function:
    LOG_VERBOSE("Executing component start function '%s' via thunk.", target_export_func->name);

    bool thunk_success = false;
    if (!thunk_ctx->host_callable_c_function_ptr) {
         set_comp_rt_error_v(error_buf, error_buf_size, "Start function '%s' thunk function pointer is NULL.", target_export_func->name);
    } else {
        WASMExecEnv *exec_env_for_thunk = thunk_ctx->parent_comp_exec_env ? thunk_ctx->parent_comp_exec_env : comp_inst->exec_env;
        if (!exec_env_for_thunk) {
             set_comp_rt_error_v(error_buf, error_buf_size, "No valid WASMExecEnv for start function '%s'.", target_export_func->name);
        } else {
            bool (*thunk_executor_func)(WASMExecEnv*, uint32, uint32[]) =
                (bool (*)(WASMExecEnv*, uint32, uint32[]))thunk_ctx->host_callable_c_function_ptr;

            thunk_success = thunk_executor_func(exec_env_for_thunk, total_thunk_argc, host_argv_for_thunk);

            if (!thunk_success) {
                // Check if exception was set by the thunk executor or the underlying call
                WASMModuleInstance *current_mod_inst = wasm_runtime_get_module_inst(exec_env_for_thunk);
                if (current_mod_inst && wasm_runtime_get_exception(current_mod_inst)) {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Start function '%s' execution failed with exception: %s",
                                       target_export_func->name, wasm_runtime_get_exception(current_mod_inst));
                    // wasm_runtime_clear_exception(current_mod_inst); // Optional: clear if handled
                } else {
                    set_comp_rt_error_v(error_buf, error_buf_size, "Start function '%s' execution failed (thunk returned false).", target_export_func->name);
                }
            } else {
                 LOG_VERBOSE("Start function '%s' executed successfully via thunk.", target_export_func->name);
            }
        }
    }

    if (host_argv_for_thunk) {
        bh_free(host_argv_for_thunk);
    }

    if (!thunk_success) {
        return false; // Error message should be set
    }
    // Clear error buffer on success if it was only used for simulation notes before
    if (error_buf_size > 0 && strstr(error_buf, "Simulating successful execution") != NULL) {
         error_buf[0] = '\0';
    }

    // Check for exceptions if the call were made (already handled if thunk_success is false and exception was set)
    // WASMModuleInstance *exception_module_inst = wasm_runtime_get_module_inst(exec_env_for_thunk);
    // if (exception_module_inst && wasm_runtime_get_exception(exception_module_inst)) {
    //     set_comp_rt_error_v(error_buf, error_buf_size, "Exception occurred during start function '%s': %s",
    //                        target_export_func->name, wasm_runtime_get_exception(exception_module_inst));
    //     wasm_runtime_clear_exception(exception_module_inst);
    //     return false;
    // }

    return true;
}
