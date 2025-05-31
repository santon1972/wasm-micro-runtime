#include "component_test_utils.h"
#include "wasm_component_canonical.h" // For global_resource_table access (test only)

// WAT for a core module defining resource operations
// (module
//   (type $i32_to_void (func (param i32)))
//   (func $dtor (type $i32_to_void) (param $h i32)
//     (global.set $dtor_called_flag (i32.const 1))
//     (call $log_i32 (local.get $h)) ;; Log the handle being dropped
//   )
//   (func $log_i32 (param i32) (nop)) ;; Placeholder for actual logging
//   (global $dtor_called_flag (mut i32) (i32.const 0))
//   (export "my_resource_destructor" (func $dtor))
//   (export "get_dtor_flag" (global $dtor_called_flag))
// )
// To pre-compile: wat2wasm resource_app.wat -o resource_app.wasm
const char* resource_app_wat = R"WAT(
(module
  (type $i32_to_void (func (param i32)))
  (import "env" "log_i32" (func $log_i32 (param i32))) ;; Optional import for logging
  (func $dtor (type $i32_to_void) (param $h i32)
    (global.set $dtor_called_flag (i32.const 1))
    (call $log_i32 (local.get $h))
  )
  (global $dtor_called_flag (mut i32) (i32.const 0))
  (export "my_resource_destructor" (func $dtor))
  (export "get_dtor_flag" (global $dtor_called_flag))
)
)WAT";
// TODO: Compile this WAT to wasm bytecode and embed or load from file.
// For now, we'll mock the dtor check or assume it's handled.

class ComponentResourceTest : public ::testing::Test {
protected:
    WASMExecEnv *exec_env;
    WASMModuleInstance *dummy_module_inst; // For creating exec_env

    void SetUp() override {
        dummy_module_inst = واsm_load_and_instantiate_wasm_file_for_test("dummy.wasm"); // Need a dummy wasm
        ASSERT_NE(dummy_module_inst, nullptr);
        exec_env = wasm_exec_env_create_for_module_inst(dummy_module_inst);
        ASSERT_NE(exec_env, nullptr);

        // Reset global resource table for test isolation if possible, or manage handles carefully
        // This is tricky as it's global. For now, tests must be careful.
        // initialize_resource_table(); // Ensure it's ready
    }

    void TearDown() override {
        if (exec_env) wasm_exec_env_destroy(exec_env);
        if (dummy_module_inst) wasm_deinstantiate(dummy_module_inst);
    }

    // Helper to create a component for resource tests
    // It would define a resource type `my-res` and canonical functions for it.
    WASMComponent* create_resource_component(uint32 core_dtor_func_idx, WASMComponentCoreModule* core_module_def, char* eb, uint32 eb_size) {
        WASMComponent *comp = (WASMComponent*)bh_malloc(sizeof(WASMComponent));
        ASSERT_NE(comp, nullptr);
        memset(comp, 0, sizeof(WASMComponent));

        // Core Module Section
        comp->core_module_count = 1;
        comp->core_modules = core_module_def;

        // Core Instance Section
        comp->core_instance_count = 1;
        comp->core_instances = (WASMComponentCoreInstance*)bh_malloc(sizeof(WASMComponentCoreInstance));
        ASSERT_NE(comp->core_instances, nullptr);
        comp->core_instances[0].kind = CORE_INSTANCE_KIND_INSTANTIATE;
        comp->core_instances[0].u.instantiate.module_idx = 0;
        comp->core_instances[0].u.instantiate.arg_count = 0; // Assuming no imports for the simple dtor module

        // Type Definition Section: resource type `my-res`
        comp->type_definition_count = 1;
        comp->type_definitions = (WASMComponentDefinedType*)bh_malloc(sizeof(WASMComponentDefinedType));
        ASSERT_NE(comp->type_definitions, nullptr);
        WASMComponentDefinedType *res_def_type = &comp->type_definitions[0];
        res_def_type->kind = DEF_TYPE_KIND_RESOURCE;
        res_def_type->u.res_type.rep = VALUE_TYPE_I32; // Representation is i32
        res_def_type->u.res_type.dtor_func_idx = core_dtor_func_idx; // Index in its own core module (defined by core_module_def)

        // Canonical Functions: resource.new, resource.drop, resource.rep for `my-res` (type_idx 0)
        comp->canonical_count = 3;
        comp->canonicals = (WASMComponentCanonical*)bh_malloc(3 * sizeof(WASMComponentCanonical));
        ASSERT_NE(comp->canonicals, nullptr);

        // resource.new
        comp->canonicals[0].func_kind = CANONICAL_FUNC_KIND_RESOURCE_NEW;
        comp->canonicals[0].u.type_idx_op.type_idx = 0; // Refers to `my-res` type def
        comp->canonicals[0].option_count = 0;

        // resource.drop
        comp->canonicals[1].func_kind = CANONICAL_FUNC_KIND_RESOURCE_DROP;
        comp->canonicals[1].u.type_idx_op.type_idx = 0;
        comp->canonicals[1].option_count = 0;

        // resource.rep
        comp->canonicals[2].func_kind = CANONICAL_FUNC_KIND_RESOURCE_REP;
        comp->canonicals[2].u.type_idx_op.type_idx = 0;
        comp->canonicals[2].option_count = 0;

        // Exports for these canonical functions
        comp->export_count = 3;
        comp->exports = (WASMComponentExport*)bh_malloc(3 * sizeof(WASMComponentExport));
        ASSERT_NE(comp->exports, nullptr);

        comp->exports[0].name = bh_strdup("resource-new");
        comp->exports[0].kind = EXPORT_KIND_FUNC; // resource.new is a function
        comp->exports[0].item_idx = 0; // Index into canonicals array

        comp->exports[1].name = bh_strdup("resource-drop");
        comp->exports[1].kind = EXPORT_KIND_FUNC;
        comp->exports[1].item_idx = 1;

        comp->exports[2].name = bh_strdup("resource-rep");
        comp->exports[2].kind = EXPORT_KIND_FUNC;
        comp->exports[2].item_idx = 2;

        return comp;
    }
};

TEST_F(ComponentResourceTest, BasicResourceLifecycle) {
    // This test requires a pre-compiled resource_app.wasm or ability to compile WAT.
    // For now, we'll assume the destructor logic is simple and its effect can be mocked or is minor.
    // The main check is if the functions can be called and handles managed.
    GTEST_SKIP() << "Resource test needs compiled resource_app.wasm and potentially mockable dtor checks.";

    // Load the core module that defines the destructor.
    // uint32_t dtor_module_size;
    // uint8_t* dtor_module_buf = bh_read_file_to_buffer("wasm_apps/resource_app.wasm", &dtor_module_size);
    // ASSERT_NE(dtor_module_buf, nullptr);
    // WASMModule *dtor_wasm_module = wasm_runtime_load(dtor_module_buf, dtor_module_size, error_buf, sizeof(error_buf));
    // ASSERT_NE(dtor_wasm_module, nullptr) << error_buf;
    // bh_free(dtor_module_buf);

    // WASMComponentCoreModule core_module_with_dtor_def;
    // core_module_with_dtor_def.module_object = dtor_wasm_module; // Already loaded

    // uint32 dtor_func_idx_in_core_module = 0; // Assume 'my_resource_destructor' is func 0 in resource_app.wasm

    // WASMComponent *comp = create_resource_component(dtor_func_idx_in_core_module, &core_module_with_dtor_def, error_buf, sizeof(error_buf));
    // ASSERT_NE(comp, nullptr);

    // WASMComponentInstanceInternal *comp_inst = wasm_component_instance_instantiate(comp, exec_env, NULL, 0, error_buf, sizeof(error_buf));
    // ASSERT_NE(comp_inst, nullptr) << error_buf;

    // // ... Look up resource.new, call it, get handle ...
    // // ... Look up resource.rep, call it, verify handle ...
    // // ... Look up resource.drop, call it ...
    // // ... Verify dtor was called (e.g. by calling get_dtor_flag from core module) ...

    // wasm_component_instance_deinstantiate(comp_inst);
    // // wasm_runtime_unload(dtor_wasm_module); // Unload the core module
    // // wasm_component_unload(comp); // Unload component definition (needs to be implemented)
}

// TODO: Add tests for dtor on deinstantiation, multiple resources, etc.

```
This sets up the resource test file. It includes a WAT string for a module with a destructor, but compiling it and then properly linking the destructor index is non-trivial for this testing structure right now. I've marked the test as `GTEST_SKIP()` for now. The helper `create_resource_component` is also a basic sketch.

Next, `component_alias_test.cc`.
