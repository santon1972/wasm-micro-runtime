#include "component_test_utils.h"
#include "wasm_component_canonical.h" // For HostComponentList, WAMRHostGeneralValue etc.

// Placeholder for a core Wasm module that has an identity function
// (param i32) (result i32)
const char *core_identity_wasm_path = "wasm_apps/core_identity.wasm";

// Simple core identity module (param i32) (result i32)
// (module
//   (func $core_identity (param i32) (result i32) local.get 0)
//   (export "core_identity" (func $core_identity))
// )
// To pre-compile: wat2wasm core_identity.wat -o core_identity.wasm
unsigned char core_identity_wasm_bytecode[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x03, 0x02, 0x01, 0x00, 0x07, 0x11, 0x01, 0x0d,
    0x63, 0x6f, 0x72, 0x65, 0x5f, 0x69, 0x64, 0x65, 0x6e, 0x74, 0x69, 0x74,
    0x79, 0x00, 0x00, 0x0a, 0x06, 0x01, 0x04, 0x00, 0x20, 0x00, 0x0b
};
uint32_t core_identity_wasm_bytecode_len = sizeof(core_identity_wasm_bytecode);


class ComponentLiftingLoweringTest : public ::testing::Test {
protected:
    WASMExecEnv *exec_env;
    WASMModuleInstance *module_inst; // For core module if needed by exec_env
    char error_buf[128];

    void SetUp() override {
        // Initialize runtime environment if needed for exec_env
        // For now, most component canonical functions get module_inst from exec_env
        // which might be NULL if called from host directly without a current core instance.
        // This setup might need adjustment based on how exec_env is used.
        exec_env = wasm_exec_env_create(NULL, 1024 * 64); // Dummy module, dummy stack size
        ASSERT_NE(exec_env, nullptr);
        // module_inst = wasm_runtime_get_module_inst(exec_env); // This will be the dummy module_inst
    }

    void TearDown() override {
        if (exec_env) {
            wasm_exec_env_destroy(exec_env);
        }
    }

    // Helper to create a simple component with one core module and one lifted/lowered function
    // The core module should export "core_identity" (i32) -> i32
    // The component will export "test_func" which is a lift/lower of this.
    WASMComponent* create_simple_component(WASMComponentValType *param_type, WASMComponentValType *result_type,
                                           WASMComponentCoreModule *core_module_def,
                                           char* error_buf, uint32_t error_buf_size) {
        WASMComponent *component = (WASMComponent*)bh_malloc(sizeof(WASMComponent));
        if (!component) {
            snprintf(error_buf, error_buf_size, "Failed to allocate component");
            return NULL;
        }
        memset(component, 0, sizeof(WASMComponent));

        // 1. Core Module Section
        component->core_module_count = 1;
        component->core_modules = core_module_def; // Externally provided

        // 2. Core Instance Section (instantiate core_module_def[0])
        component->core_instance_count = 1;
        component->core_instances = (WASMComponentCoreInstance*)bh_malloc(sizeof(WASMComponentCoreInstance));
        ASSERT_NE(component->core_instances, nullptr);
        memset(component->core_instances, 0, sizeof(WASMComponentCoreInstance));
        component->core_instances[0].kind = CORE_INSTANCE_KIND_INSTANTIATE;
        component->core_instances[0].u.instantiate.module_idx = 0;
        component->core_instances[0].u.instantiate.arg_count = 0;
        component->core_instances[0].u.instantiate.args = NULL;

        // 3. Alias core_identity export from core instance 0
        component->alias_count = 1;
        component->aliases = (WASMComponentAlias*)bh_malloc(sizeof(WASMComponentAlias));
        ASSERT_NE(component->aliases, nullptr);
        component->aliases[0].sort = ALIAS_SORT_CORE_FUNC;
        component->aliases[0].target_kind = ALIAS_TARGET_CORE_EXPORT;
        component->aliases[0].target_outer_depth = 0; // Not used for core export
        component->aliases[0].target_idx = 0; // Core instance index
        component->aliases[0].target_name = bh_strdup("core_identity");
        ASSERT_NE(component->aliases[0].target_name, nullptr);

        // 4. Component Function Type Definition (param_type) -> result_type
        component->type_definition_count = 1;
        component->type_definitions = (WASMComponentDefinedType*)bh_malloc(sizeof(WASMComponentDefinedType));
        ASSERT_NE(component->type_definitions, nullptr);
        component->type_definitions[0].kind = DEF_TYPE_KIND_FUNC;
        WASMComponentFuncType *func_type = &component->type_definitions[0].u.func_type;
        func_type->param_count = param_type ? 1 : 0; // Assuming single param for simplicity now
        if (param_type) {
            func_type->params = (WASMComponentLabelValType*)bh_malloc(sizeof(WASMComponentLabelValType));
            ASSERT_NE(func_type->params, nullptr);
            func_type->params[0].label = bh_strdup("arg0");
            ASSERT_NE(func_type->params[0].label, nullptr);
            func_type->params[0].valtype = param_type; // Shallow copy of type
        } else {
            func_type->params = NULL;
        }
        func_type->result = result_type; // Shallow copy of type

        // 5. Canonical Lift definition for the function
        component->canonical_count = 1;
        component->canonicals = (WASMComponentCanonical*)bh_malloc(sizeof(WASMComponentCanonical));
        ASSERT_NE(component->canonicals, nullptr);
        component->canonicals[0].func_kind = CANONICAL_FUNC_KIND_LIFT;
        component->canonicals[0].u.lift.core_func_idx = 0; // Alias index for core_identity
        component->canonicals[0].u.lift.component_func_type_idx = 0; // Type def index for (param_type)->result_type
        component->canonicals[0].option_count = 0;
        component->canonicals[0].options = NULL;
        // TODO: Add options for string encodings etc.

        // 6. Export the lifted function
        component->export_count = 1;
        component->exports = (WASMComponentExport*)bh_malloc(sizeof(WASMComponentExport));
        ASSERT_NE(component->exports, nullptr);
        component->exports[0].name = bh_strdup("test_func");
        ASSERT_NE(component->exports[0].name, nullptr);
        component->exports[0].kind = EXPORT_KIND_FUNC;
        component->exports[0].item_idx = 0; // Index of the canonical_def above
        component->exports[0].optional_desc_type_idx = (uint32)-1; // No explicit type annotation on export

        return component;
    }

    void unload_simple_component(WASMComponent *component) {
        if (!component) return;
        if (component->exports) {
            if (component->exports[0].name) bh_free(component->exports[0].name);
            bh_free(component->exports);
        }
        if (component->canonicals) bh_free(component->canonicals);
        if (component->type_definitions) {
            if (component->type_definitions[0].u.func_type.params) {
                bh_free(component->type_definitions[0].u.func_type.params[0].label);
                bh_free(component->type_definitions[0].u.func_type.params);
            }
            bh_free(component->type_definitions);
        }
        if (component->aliases) {
            if (component->aliases[0].target_name) bh_free(component->aliases[0].target_name);
            bh_free(component->aliases);
        }
        if (component->core_instances) bh_free(component->core_instances);
        // Core modules are managed by the caller which provides core_module_def
        bh_free(component);
    }
};


TEST_F(ComponentLiftingLoweringTest, LiftLowerS32) {
    WASMComponentValType s32_val_type;
    s32_val_type.kind = VAL_TYPE_KIND_PRIMITIVE;
    s32_val_type.u.primitive = PRIM_VAL_S32;

    WASMComponentCoreModule core_module_def;
    core_module_def.module_data = core_identity_wasm_bytecode;
    core_module_def.module_len = core_identity_wasm_bytecode_len;
    core_module_def.module_object = NULL; // Loaded by runtime

    WASMComponent *component = create_simple_component(&s32_val_type, &s32_val_type, &core_module_def, error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    WASMComponentInstanceInternal *comp_inst = wasm_component_instance_instantiate(component, exec_env, NULL, 0, error_buf, sizeof(error_buf));
    ASSERT_NE(comp_inst, nullptr) << error_buf;
    ASSERT_NE(comp_inst->module_instances[0], nullptr); // Core module should be instantiated

    ResolvedComponentExportItem *export_item = find_export_by_name(comp_inst, "test_func");
    ASSERT_NE(export_item, nullptr);
    ASSERT_EQ(export_item->kind, COMPONENT_EXPORT_KIND_FUNC);
    ASSERT_NE(export_item->item.function_thunk_context, nullptr);

    LiftedFuncThunkContext *thunk_ctx = (LiftedFuncThunkContext*)export_item->item.function_thunk_context;
    bool (*thunk_func)(WASMExecEnv*, uint32, uint32[]) = (bool(*)(WASMExecEnv*, uint32, uint32[]))thunk_ctx->host_callable_c_function_ptr;

    int32_t input_val = 12345;
    int32_t output_val = 0;
    uint32_t argv[3]; // context, arg0_ptr, result_ptr
    argv[0] = (uint32_t)(uintptr_t)thunk_ctx;
    argv[1] = (uint32_t)(uintptr_t)&input_val;
    argv[2] = (uint32_t)(uintptr_t)&output_val;

    ASSERT_TRUE(thunk_func(comp_inst->exec_env, 3, argv));
    ASSERT_EQ(output_val, input_val);

    wasm_component_instance_deinstantiate(comp_inst);
    unload_simple_component(component); // Does not free core_module_def.module_object, runtime does
}

// TODO: Add tests for string (UTF-8, UTF-16, Latin1+UTF16), list, record, tuple, option, result, flags, enum, bool, char, floats, u32, s64, u64.

TEST_F(ComponentLiftingLoweringTest, LiftLowerStringUTF8) {
    // Setup string valtype
    WASMComponentValType string_val_type;
    string_val_type.kind = VAL_TYPE_KIND_PRIMITIVE;
    string_val_type.u.primitive = PRIM_VAL_STRING;

    // Setup core module
    WASMComponentCoreModule core_module_def;
    core_module_def.module_data = core_identity_wasm_bytecode; // Placeholder, string identity needs (ptr,len)->(ptr,len)
    core_module_def.module_len = core_identity_wasm_bytecode_len;
    core_module_def.module_object = NULL;

    // For string, core_identity (i32,i32)->(i32,i32) is needed. The current one is (i32)->(i32)
    // This test will need a different core_identity wasm or skip for now.
    GTEST_SKIP() << "Skipping string test as core_identity for strings is not yet set up.";


    WASMComponent *component = create_simple_component(&string_val_type, &string_val_type, &core_module_def, error_buf, sizeof(error_buf));
    // ... rest of test ...
    if (component) unload_simple_component(component);
}
```

This sets up the first test file with a basic s32 lift/lower test and a placeholder for string tests. The `create_simple_component` helper is a bit complex but needed to construct components programmatically for testing specific types.
The core identity WASM is embedded for now. Ideally, these would be separate `.wat`/`.wasm` files.

Next, `component_resource_test.cc`.
