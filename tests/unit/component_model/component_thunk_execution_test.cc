#include "component_test_utils.h"
#include "wasm_component_canonical.h" // For LiftedFuncThunkContext

// Using core_identity_wasm_bytecode and len from component_lifting_lowering_test.cc
// (module
//   (func $add (param i32 i32) (result i32) local.get 0 local.get 1 call $add_import)
//   (import "env" "add_import" (func $add_import (param i32 i32) (result i32)))
//   (export "core_add" (func $add))
// )
// For this test, we'll use a simpler core_add that doesn't rely on further imports.
// (module
//   (func $core_add (param i32 i32) (result i32) local.get 0 local.get 1 i32.add)
//   (export "core_add" (func $core_add))
// )
// wat2wasm core_add.wat -o core_add.wasm
unsigned char core_add_wasm_bytecode[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x0c, 0x01,
    0x08, 0x63, 0x6f, 0x72, 0x65, 0x5f, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a,
    0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b
};
uint32_t core_add_wasm_bytecode_len = sizeof(core_add_wasm_bytecode);


class ComponentThunkExecutionTest : public ::testing::Test {
protected:
    WASMExecEnv *exec_env;
    // No dummy_module_inst needed if component instantiation creates its own.

    void SetUp() override {
        // exec_env for the main test thread, not necessarily passed directly to component
        // exec_env = wasm_exec_env_create(NULL, 1024 * 64); // Dummy module, dummy stack size
        // ASSERT_NE(exec_env, nullptr);
    }

    void TearDown() override {
        // if (exec_env) wasm_exec_env_destroy(exec_env);
    }

    // Helper to create a component that lifts core_add
    WASMComponent* create_add_component(char* error_buf, uint32_t error_buf_size) {
        WASMComponent *component = (WASMComponent*)bh_malloc(sizeof(WASMComponent));
        ASSERT_NE(component, nullptr);
        memset(component, 0, sizeof(WASMComponent));

        // 1. Core Module Section (core_add.wasm)
        component->core_module_count = 1;
        component->core_modules = (WASMComponentCoreModule*)bh_malloc(sizeof(WASMComponentCoreModule));
        ASSERT_NE(component->core_modules, nullptr);
        component->core_modules[0].module_data = core_add_wasm_bytecode;
        component->core_modules[0].module_len = core_add_wasm_bytecode_len;
        component->core_modules[0].module_object = NULL;

        // 2. Core Instance Section
        component->core_instance_count = 1;
        component->core_instances = (WASMComponentCoreInstance*)bh_malloc(sizeof(WASMComponentCoreInstance));
        ASSERT_NE(component->core_instances, nullptr);
        component->core_instances[0].kind = CORE_INSTANCE_KIND_INSTANTIATE;
        component->core_instances[0].u.instantiate.module_idx = 0;
        component->core_instances[0].u.instantiate.arg_count = 0;

        // 3. Alias "core_add" from core instance 0
        component->alias_count = 1;
        component->aliases = (WASMComponentAlias*)bh_malloc(sizeof(WASMComponentAlias));
        ASSERT_NE(component->aliases, nullptr);
        component->aliases[0].sort = ALIAS_SORT_CORE_FUNC;
        component->aliases[0].target_kind = ALIAS_TARGET_CORE_EXPORT;
        component->aliases[0].target_idx = 0; // core instance definition index
        component->aliases[0].target_name = bh_strdup("core_add");
        ASSERT_NE(component->aliases[0].target_name, nullptr);

        // 4. Component Function Type: (s32, s32) -> s32
        component->type_definition_count = 1;
        component->type_definitions = (WASMComponentDefinedType*)bh_malloc(sizeof(WASMComponentDefinedType));
        ASSERT_NE(component->type_definitions, nullptr);
        WASMComponentDefinedType *func_type_def = &component->type_definitions[0];
        func_type_def->kind = DEF_TYPE_KIND_FUNC;
        WASMComponentFuncType *comp_func_type = &func_type_def->u.func_type;
        comp_func_type->param_count = 2;
        comp_func_type->params = (WASMComponentLabelValType*)bh_malloc(2 * sizeof(WASMComponentLabelValType));
        ASSERT_NE(comp_func_type->params, nullptr);
        comp_func_type->params[0].label = bh_strdup("a");
        comp_func_type->params[0].valtype = (WASMComponentValType*)bh_malloc(sizeof(WASMComponentValType));
        comp_func_type->params[0].valtype->kind = VAL_TYPE_KIND_PRIMITIVE;
        comp_func_type->params[0].valtype->u.primitive = PRIM_VAL_S32;
        comp_func_type->params[1].label = bh_strdup("b");
        comp_func_type->params[1].valtype = (WASMComponentValType*)bh_malloc(sizeof(WASMComponentValType));
        comp_func_type->params[1].valtype->kind = VAL_TYPE_KIND_PRIMITIVE;
        comp_func_type->params[1].valtype->u.primitive = PRIM_VAL_S32;
        comp_func_type->result = (WASMComponentValType*)bh_malloc(sizeof(WASMComponentValType));
        ASSERT_NE(comp_func_type->result, nullptr);
        comp_func_type->result->kind = VAL_TYPE_KIND_PRIMITIVE;
        comp_func_type->result->u.primitive = PRIM_VAL_S32;

        // 5. Canonical Lift definition for "core_add"
        component->canonical_count = 1;
        component->canonicals = (WASMComponentCanonical*)bh_malloc(sizeof(WASMComponentCanonical));
        ASSERT_NE(component->canonicals, nullptr);
        component->canonicals[0].func_kind = CANONICAL_FUNC_KIND_LIFT;
        component->canonicals[0].u.lift.core_func_idx = 0; // Alias index for core_add
        component->canonicals[0].u.lift.component_func_type_idx = 0; // Type def index for (s32,s32)->s32
        component->canonicals[0].option_count = 0;

        // 6. Export the lifted function
        component->export_count = 1;
        component->exports = (WASMComponentExport*)bh_malloc(sizeof(WASMComponentExport));
        ASSERT_NE(component->exports, nullptr);
        component->exports[0].name = bh_strdup("component_add");
        ASSERT_NE(component->exports[0].name, nullptr);
        component->exports[0].kind = EXPORT_KIND_FUNC;
        component->exports[0].item_idx = 0; // Index of the canonical_def above
        component->exports[0].optional_desc_type_idx = (uint32)-1;

        return component;
    }

     void unload_add_component(WASMComponent *component) {
        if (!component) return;
        if (component->exports) { bh_free(component->exports[0].name); bh_free(component->exports); }
        if (component->canonicals) bh_free(component->canonicals);
        if (component->type_definitions) {
            WASMComponentFuncType *ft = &component->type_definitions[0].u.func_type;
            bh_free(ft->params[0].label); bh_free(ft->params[0].valtype);
            bh_free(ft->params[1].label); bh_free(ft->params[1].valtype);
            bh_free(ft->params);
            bh_free(ft->result);
            bh_free(component->type_definitions);
        }
        if (component->aliases) { bh_free(component->aliases[0].target_name); bh_free(component->aliases); }
        if (component->core_instances) bh_free(component->core_instances);
        if (component->core_modules) bh_free(component->core_modules); // module_object is handled by runtime
        bh_free(component);
    }
};

TEST_F(ComponentThunkExecutionTest, LiftedAddFunction) {
    char error_buf[256] = {0};
    WASMComponent *component = create_add_component(error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    // Create a dummy parent exec_env for the component instance
    // The actual module_inst for this exec_env doesn't matter much for this test,
    // as long as the exec_env itself is valid for wasm_component_instance_instantiate.
    // In a real host, this might be the exec_env of a calling component or a base host env.
    WASMModuleInstance *dummy_mod = wasm_runtime_load(core_add_wasm_bytecode, core_add_wasm_bytecode_len, error_buf, sizeof(error_buf));
    ASSERT_NE(dummy_mod, nullptr) << error_buf;
    WASMExecEnv *parent_exec_env = wasm_exec_env_create(dummy_mod, 1024);
    ASSERT_NE(parent_exec_env, nullptr);


    WASMComponentInstanceInternal *comp_inst = wasm_component_instance_instantiate(component, parent_exec_env, NULL, 0, error_buf, sizeof(error_buf));
    ASSERT_NE(comp_inst, nullptr) << error_buf;
    ASSERT_NE(comp_inst->module_instances[0], nullptr);

    ResolvedComponentExportItem *export_item = find_export_by_name(comp_inst, "component_add");
    ASSERT_NE(export_item, nullptr);
    ASSERT_EQ(export_item->kind, COMPONENT_EXPORT_KIND_FUNC);
    ASSERT_NE(export_item->item.function_thunk_context, nullptr);

    LiftedFuncThunkContext *thunk_ctx = (LiftedFuncThunkContext*)export_item->item.function_thunk_context;
    bool (*thunk_func)(WASMExecEnv*, uint32, uint32[]) = (bool(*)(WASMExecEnv*, uint32, uint32[]))thunk_ctx->host_callable_c_function_ptr;

    int32_t arg1 = 100;
    int32_t arg2 = 23;
    int32_t result = 0;
    int32_t* result_ptr = &result; // So the thunk can write to it

    // argv for thunk: context, ptr_to_arg1, ptr_to_arg2, ptr_to_ptr_to_result
    uint32_t argv[4];
    argv[0] = (uint32_t)(uintptr_t)thunk_ctx;
    argv[1] = (uint32_t)(uintptr_t)&arg1;
    argv[2] = (uint32_t)(uintptr_t)&arg2;
    argv[3] = (uint32_t)(uintptr_t)&result_ptr; // Pass address of the pointer where result should be stored

    // The thunk will be called by some runtime mechanism, for testing we call it directly.
    // The exec_env passed to the thunk should be the one associated with the component instance or its caller.
    // Using comp_inst->exec_env (which is parent_exec_env here).
    ASSERT_TRUE(thunk_func(comp_inst->exec_env, 4, argv)) << wasm_runtime_get_exception(comp_inst->module_instances[0]);
    ASSERT_EQ(result, 123);

    wasm_component_instance_deinstantiate(comp_inst);
    unload_add_component(component);
    wasm_exec_env_destroy(parent_exec_env);
    wasm_runtime_unload(dummy_mod);
}
```

This sets up the thunk execution test with a simple `add` function. The `create_add_component` helper builds the necessary component structure. The test directly invokes the thunk C function pointer.

I've created the skeletons and some initial test cases. This is a foundational step. Many more tests and helper refinements would be needed for full coverage. The WAT/WASM files would ideally be external and compiled as part of the build. The current approach embeds small bytecode arrays for simplicity in this context.

I will now submit this initial set of test files.
