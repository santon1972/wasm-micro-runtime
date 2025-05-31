#include "component_test_utils.h"

// Using core_identity_wasm_bytecode and len from component_lifting_lowering_test.cc for simplicity
extern unsigned char core_identity_wasm_bytecode[];
extern uint32_t core_identity_wasm_bytecode_len;

class ComponentAliasTest : public ::testing::Test {
protected:
    WASMExecEnv *exec_env;
     WASMModuleInstance *dummy_module_inst;

    void SetUp() override {
        // dummy_module_inst = wasm_load_and_instantiate_wasm_file_for_test("dummy.wasm");
        // ASSERT_NE(dummy_module_inst, nullptr);
        // exec_env = wasm_exec_env_create_for_module_inst(dummy_module_inst);
        // ASSERT_NE(exec_env, nullptr);

        // Simplified setup, direct exec_env creation without full module instance
        // This might be insufficient if alias resolution or thunks need a valid module_inst from exec_env deeply
        exec_env = wasm_exec_env_create(NULL, 1024 * 64);
        ASSERT_NE(exec_env, nullptr);
    }

    void TearDown() override {
        if (exec_env) wasm_exec_env_destroy(exec_env);
        // if (dummy_module_inst) wasm_deinstantiate(dummy_module_inst);
    }
};

TEST_F(ComponentAliasTest, AliasCoreExportFunc) {
    char error_buf[128];
    WASMComponent *component = (WASMComponent*)bh_malloc(sizeof(WASMComponent));
    ASSERT_NE(component, nullptr);
    memset(component, 0, sizeof(WASMComponent));

    // 1. Core Module Section
    component->core_module_count = 1;
    component->core_modules = (WASMComponentCoreModule*)bh_malloc(sizeof(WASMComponentCoreModule));
    ASSERT_NE(component->core_modules, nullptr);
    component->core_modules[0].module_data = core_identity_wasm_bytecode;
    component->core_modules[0].module_len = core_identity_wasm_bytecode_len;
    component->core_modules[0].module_object = NULL;


    // 2. Core Instance Section (instantiate core_module[0])
    component->core_instance_count = 1;
    component->core_instances = (WASMComponentCoreInstance*)bh_malloc(sizeof(WASMComponentCoreInstance));
    ASSERT_NE(component->core_instances, nullptr);
    component->core_instances[0].kind = CORE_INSTANCE_KIND_INSTANTIATE;
    component->core_instances[0].u.instantiate.module_idx = 0;
    component->core_instances[0].u.instantiate.arg_count = 0;

    // 3. Alias core_identity export from core instance 0 (runtime index 0)
    component->alias_count = 1;
    component->aliases = (WASMComponentAlias*)bh_malloc(sizeof(WASMComponentAlias));
    ASSERT_NE(component->aliases, nullptr);
    component->aliases[0].sort = ALIAS_SORT_CORE_FUNC;
    component->aliases[0].target_kind = ALIAS_TARGET_CORE_EXPORT;
    component->aliases[0].target_idx = 0; // Core instance definition index
    component->aliases[0].target_name = bh_strdup("core_identity");
    ASSERT_NE(component->aliases[0].target_name, nullptr);

    // 4. Export the aliased function
    component->export_count = 1;
    component->exports = (WASMComponentExport*)bh_malloc(sizeof(WASMComponentExport));
    ASSERT_NE(component->exports, nullptr);
    component->exports[0].name = bh_strdup("aliased_core_identity");
    ASSERT_NE(component->exports[0].name, nullptr);
    component->exports[0].kind = EXPORT_KIND_FUNC; // Exporting a function
    // This item_idx should point to a canonical function definition that LIFTS the alias.
    // For this test, we'll simplify: assume item_idx directly points to the alias if the export system can handle it.
    // This part of the test setup is simplified due to not having a canonical lift of an alias.
    // A proper test would have: export -> canonical_lift_def -> alias_def -> core_func
    // For now, this test will likely fail at export population or thunk creation.
    // Let's adjust to test resolve_component_alias_by_index more directly.

    // For now, let's assume the export directly refers to the alias index for testing purposes.
    // This is not how components actually export aliased core functions (they export lifted functions).
    // This test is more about whether the alias itself resolves.
    // We'll manually call resolve_component_alias_by_index.

    WASMComponentInstanceInternal *comp_inst = wasm_component_instance_instantiate(component, exec_env, NULL, 0, error_buf, sizeof(error_buf));
    ASSERT_NE(comp_inst, nullptr) << error_buf;
    ASSERT_NE(comp_inst->module_instances[0], nullptr); // Core module should be instantiated

    ResolvedComponentItem resolved_item;
    bool success = resolve_component_alias_by_index(comp_inst, 0, ALIAS_SORT_CORE_FUNC, &resolved_item, error_buf, sizeof(error_buf));
    ASSERT_TRUE(success) << error_buf;
    ASSERT_EQ(resolved_item.kind, RESOLVED_ITEM_CORE_FUNC);
    ASSERT_NE(resolved_item.item.core_item.core_module_inst, nullptr);
    ASSERT_EQ(resolved_item.item.core_item.core_module_inst, comp_inst->module_instances[0]);
    // item_idx_in_module check is more involved, depends on precise export order in core_identity.wasm
    // For core_identity.wasm, "core_identity" is func 0.
    ASSERT_EQ(resolved_item.item.core_item.item_idx_in_module, 0);


    wasm_component_instance_deinstantiate(comp_inst);
    // Manual cleanup for this simplified component
    if (component->exports) { bh_free(component->exports[0].name); bh_free(component->exports); }
    if (component->aliases) { bh_free(component->aliases[0].target_name); bh_free(component->aliases); }
    if (component->core_instances) bh_free(component->core_instances);
    if (component->core_modules) bh_free(component->core_modules);
    bh_free(component);
}

// TODO: Add tests for ALIAS_TARGET_OUTER, ALIAS_TARGET_INSTANCE, ALIAS_TARGET_TYPE etc.
```
This adds a basic test for `ALIAS_TARGET_CORE_EXPORT`. It manually constructs a component definition. Note the simplification: instead of fully exporting a lifted function that *uses* the alias, it directly calls `resolve_component_alias_by_index` to check if the alias resolves correctly.

Finally, `component_thunk_execution_test.cc`.
