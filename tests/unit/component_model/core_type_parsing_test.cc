#include "gtest/gtest.h"

extern "C" {
#include "wasm_component_loader.h"
#include "wasm_export.h" // For VALUE_TYPE_xxx and WASM_EXTERNAL_xxx
// CORE_TYPE_KIND_MODULE is defined in wasm_component_loader.h
}

// Helper function to construct a minimal component with a core type section
static std::vector<uint8_t>
build_component(const std::vector<uint8_t> &core_type_section_payload)
{
    std::vector<uint8_t> component_bytes;
    // Component Header: magic, version, layer
    component_bytes.insert(component_bytes.end(),
                           { 0x00, 0x61, 0x73, 0x6D }); // magic
    component_bytes.insert(component_bytes.end(),
                           { 0x0D, 0x00 }); // version (draft 13)
    component_bytes.insert(component_bytes.end(), { 0x01, 0x00 }); // layer 1

    // Core Type Section Header
    component_bytes.push_back(COMPONENT_SECTION_ID_CORE_TYPE); // Section ID 2

    // Section size (LEB128 encoded)
    uint32_t section_size = core_type_section_payload.size();
    // Simple LEB128 encoding for small sizes (up to 127)
    if (section_size > 127) {
        // For simplicity, this helper only supports small section sizes for now
        // Extend LEB128 encoding if larger test payloads are needed
        GTEST_FAIL() << "Section size too large for simple LEB128 in test helper";
    }
    component_bytes.push_back(static_cast<uint8_t>(section_size));

    // Section payload
    component_bytes.insert(component_bytes.end(),
                           core_type_section_payload.begin(),
                           core_type_section_payload.end());
    return component_bytes;
}

class CoreTypeParsingTest : public ::testing::Test {
  protected:
    char error_buf[128];
    WASMComponent *component = nullptr;

    void TearDown() override
    {
        if (component) {
            wasm_component_unload(component);
            component = nullptr;
        }
    }
};

TEST_F(CoreTypeParsingTest, CoreFuncTypeSimple)
{
    // Core Type Section: 1 type definition
    // Type 0: core func type, params [i32, f64], results [i64]
    std::vector<uint8_t> core_type_payload = {
        0x01,       // Number of types in section: 1
        0x60,       // Kind: core function type
        0x02,       // Param count: 2
        (uint8_t)VALUE_TYPE_I32, // Param 1: i32
        (uint8_t)VALUE_TYPE_F64, // Param 2: f64
        0x01,       // Result count: 1
        (uint8_t)VALUE_TYPE_I64  // Result 1: i64
    };

    std::vector<uint8_t> binary = build_component(core_type_payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->core_type_def_count, 1);
    WASMComponentCoreTypeDef *type_def = &component->core_type_defs[0];
    ASSERT_EQ(type_def->kind, 0x60);
    ASSERT_NE(type_def->u.core_func_type, nullptr);

    WASMComponentCoreFuncType *func_type = type_def->u.core_func_type;
    ASSERT_EQ(func_type->param_count, 2);
    ASSERT_NE(func_type->param_types, nullptr);
    EXPECT_EQ(func_type->param_types[0], (uint8_t)VALUE_TYPE_I32);
    EXPECT_EQ(func_type->param_types[1], (uint8_t)VALUE_TYPE_F64);

    ASSERT_EQ(func_type->result_count, 1);
    ASSERT_NE(func_type->result_types, nullptr);
    EXPECT_EQ(func_type->result_types[0], (uint8_t)VALUE_TYPE_I64);
}

TEST_F(CoreTypeParsingTest, CoreFuncTypeEmpty)
{
    // Core Type Section: 1 type definition
    // Type 0: core func type, params [], results []
    std::vector<uint8_t> core_type_payload = {
        0x01, // Number of types in section: 1
        0x60, // Kind: core function type
        0x00, // Param count: 0
        0x00  // Result count: 0
    };

    std::vector<uint8_t> binary = build_component(core_type_payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->core_type_def_count, 1);
    WASMComponentCoreTypeDef *type_def = &component->core_type_defs[0];
    ASSERT_EQ(type_def->kind, 0x60);
    ASSERT_NE(type_def->u.core_func_type, nullptr);

    WASMComponentCoreFuncType *func_type = type_def->u.core_func_type;
    ASSERT_EQ(func_type->param_count, 0);
    EXPECT_EQ(func_type->param_types, nullptr);
    ASSERT_EQ(func_type->result_count, 0);
    EXPECT_EQ(func_type->result_types, nullptr);
}

TEST_F(CoreTypeParsingTest, CoreFuncTypeMultiple)
{
    // Core Type Section: 2 type definitions
    // Type 0: core func type, params [i32], results [f32]
    // Type 1: core func type, params [], results []
    std::vector<uint8_t> core_type_payload = {
        0x02,       // Number of types in section: 2
        // Type 0
        0x60,       // Kind: core function type
        0x01,       // Param count: 1
        (uint8_t)VALUE_TYPE_I32, // Param 1: i32
        0x01,       // Result count: 1
        (uint8_t)VALUE_TYPE_F32, // Result 1: f32
        // Type 1
        0x60, // Kind: core function type
        0x00, // Param count: 0
        0x00  // Result count: 0
    };

    std::vector<uint8_t> binary = build_component(core_type_payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->core_type_def_count, 2);

    // Check Type 0
    WASMComponentCoreTypeDef *type_def0 = &component->core_type_defs[0];
    ASSERT_EQ(type_def0->kind, 0x60);
    ASSERT_NE(type_def0->u.core_func_type, nullptr);
    WASMComponentCoreFuncType *func_type0 = type_def0->u.core_func_type;
    ASSERT_EQ(func_type0->param_count, 1);
    ASSERT_NE(func_type0->param_types, nullptr);
    EXPECT_EQ(func_type0->param_types[0], (uint8_t)VALUE_TYPE_I32);
    ASSERT_EQ(func_type0->result_count, 1);
    ASSERT_NE(func_type0->result_types, nullptr);
    EXPECT_EQ(func_type0->result_types[0], (uint8_t)VALUE_TYPE_F32);

    // Check Type 1
    WASMComponentCoreTypeDef *type_def1 = &component->core_type_defs[1];
    ASSERT_EQ(type_def1->kind, 0x60);
    ASSERT_NE(type_def1->u.core_func_type, nullptr);
    WASMComponentCoreFuncType *func_type1 = type_def1->u.core_func_type;
    ASSERT_EQ(func_type1->param_count, 0);
    EXPECT_EQ(func_type1->param_types, nullptr);
    ASSERT_EQ(func_type1->result_count, 0);
    EXPECT_EQ(func_type1->result_types, nullptr);
}

// Helper to create a string payload (LEB128 size + string)
static std::vector<uint8_t> string_payload(const std::string &s)
{
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(s.length())); // LEB128 size (simple for short strings)
    payload.insert(payload.end(), s.begin(), s.end());
    return payload;
}

TEST_F(CoreTypeParsingTest, CoreModuleTypeSimple)
{
    // Core Type Section: 1 type definition
    // Type 0: core module type
    //   Import: "env"."host_func", kind func, type_idx 0 (referring to a hypothetical core func type)
    //   Export: "mod_export_func", kind func, type_idx 1 (referring to a hypothetical core func type)
    std::vector<uint8_t> core_type_payload = {
        0x01, // Number of types in section: 1
        CORE_TYPE_KIND_MODULE, // Kind: core module type (0x50)
        0x02, // Declaration count: 2 (1 import, 1 export)
        // Import 0
        0x00, // core:moduledecl kind: import
    };
    std::vector<uint8_t> env_name = string_payload("env");
    std::vector<uint8_t> host_func_name = string_payload("host_func");
    core_type_payload.insert(core_type_payload.end(), env_name.begin(), env_name.end());
    core_type_payload.insert(core_type_payload.end(), host_func_name.begin(), host_func_name.end());
    core_type_payload.push_back(WASM_EXTERNAL_FUNCTION); // kind: func
    core_type_payload.push_back(0x00); // type_idx: 0

    // Export 0
    core_type_payload.push_back(0x03); // core:moduledecl kind: export
    std::vector<uint8_t> mod_export_name = string_payload("mod_export_func");
    core_type_payload.insert(core_type_payload.end(), mod_export_name.begin(), mod_export_name.end());
    core_type_payload.push_back(WASM_EXTERNAL_FUNCTION); // kind: func
    core_type_payload.push_back(0x01); // type_idx: 1


    std::vector<uint8_t> binary = build_component(core_type_payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;
    ASSERT_EQ(component->core_type_def_count, 1);
    WASMComponentCoreTypeDef *type_def = &component->core_type_defs[0];
    ASSERT_EQ(type_def->kind, CORE_TYPE_KIND_MODULE);
    ASSERT_NE(type_def->u.module_type, nullptr);

    WASMComponentCoreModuleType *mod_type = type_def->u.module_type;
    ASSERT_EQ(mod_type->import_count, 1);
    ASSERT_NE(mod_type->imports, nullptr);
    EXPECT_STREQ(mod_type->imports[0].module_name, "env");
    EXPECT_STREQ(mod_type->imports[0].field_name, "host_func");
    EXPECT_EQ(mod_type->imports[0].kind, WASM_EXTERNAL_FUNCTION);
    EXPECT_EQ(mod_type->imports[0].type_idx, 0);

    ASSERT_EQ(mod_type->export_count, 1);
    ASSERT_NE(mod_type->exports, nullptr);
    EXPECT_STREQ(mod_type->exports[0].name, "mod_export_func");
    EXPECT_EQ(mod_type->exports[0].kind, WASM_EXTERNAL_FUNCTION);
    EXPECT_EQ(mod_type->exports[0].type_idx, 1);
}


TEST_F(CoreTypeParsingTest, CoreModuleTypeEmpty)
{
    // Core Type Section: 1 type definition
    // Type 0: core module type, 0 declarations
    std::vector<uint8_t> core_type_payload = {
        0x01, // Number of types in section: 1
        CORE_TYPE_KIND_MODULE, // Kind: core module type (0x50)
        0x00  // Declaration count: 0
    };

    std::vector<uint8_t> binary = build_component(core_type_payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->core_type_def_count, 1);
    WASMComponentCoreTypeDef *type_def = &component->core_type_defs[0];
    ASSERT_EQ(type_def->kind, CORE_TYPE_KIND_MODULE);
    ASSERT_NE(type_def->u.module_type, nullptr);

    WASMComponentCoreModuleType *mod_type = type_def->u.module_type;
    ASSERT_EQ(mod_type->import_count, 0);
    EXPECT_EQ(mod_type->imports, nullptr);
    ASSERT_EQ(mod_type->export_count, 0);
    EXPECT_EQ(mod_type->exports, nullptr);
}

TEST_F(CoreTypeParsingTest, CoreModuleTypeInvalidDeclAlias)
{
    // Core Type Section: 1 type definition
    // Type 0: core module type
    //   Declaration 0: alias (kind 0x02), which is unsupported by current parser
    std::vector<uint8_t> core_type_payload = {
        0x01, // Number of types in section: 1
        CORE_TYPE_KIND_MODULE, // Kind: core module type (0x50)
        0x01, // Declaration count: 1
        0x02  // core:moduledecl kind: alias (unsupported)
        // Minimal data for a hypothetical alias to make the section size consistent if needed.
        // The loader should error out before trying to parse these.
        // For example, if it expects a type index after: 0x00
    };

    std::vector<uint8_t> binary = build_component(core_type_payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    // Expect loading to fail
    ASSERT_EQ(component, nullptr);
    // Check for specific error message if possible (might be too brittle)
    // Example: EXPECT_STRCONTAINS(error_buf, "Unsupported core:moduledecl kind 0x02 (alias)");
    // For now, just ensure it fails. The worker report indicated:
    // "Skipping core:moduledecl kind 0x02 (alias) is not supported."
    // "set_error_buf(error_buf, error_buf_size, "Unsupported core:moduledecl kind 0x02 (alias)");"
    EXPECT_NE(std::string(error_buf).find("Unsupported core:moduledecl kind 0x02 (alias)"), std::string::npos)
        << "Error message was: " << error_buf;

}

TEST_F(CoreTypeParsingTest, CoreModuleTypeDeclSkipType)
{
    // Core Type Section: 1 type definition
    // Type 0: core module type
    //   Declaration 0: type (kind 0x01), which should be skipped
    //   Declaration 1: export "test_export"
    std::vector<uint8_t> core_type_payload = {
        0x01, // Number of types in section: 1
        CORE_TYPE_KIND_MODULE, // Kind: core module type (0x50)
        0x02, // Declaration count: 2
        // Decl 0: type (skip)
        0x01, // core:moduledecl kind: type
        0x7F, // dummy type_idx (u32 LEB, 0x7F is fine for this)
        // Decl 1: export
        0x03, // core:moduledecl kind: export
    };
    std::vector<uint8_t> export_name = string_payload("test_export");
    core_type_payload.insert(core_type_payload.end(), export_name.begin(), export_name.end());
    core_type_payload.push_back(WASM_EXTERNAL_GLOBAL); // kind: global
    core_type_payload.push_back(0x05); // type_idx: 5 (dummy)

    std::vector<uint8_t> binary = build_component(core_type_payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->core_type_def_count, 1);
    WASMComponentCoreTypeDef *type_def = &component->core_type_defs[0];
    ASSERT_EQ(type_def->kind, CORE_TYPE_KIND_MODULE);
    ASSERT_NE(type_def->u.module_type, nullptr);

    WASMComponentCoreModuleType *mod_type = type_def->u.module_type;
    ASSERT_EQ(mod_type->import_count, 0); // Skipped type decl is not an import
    ASSERT_EQ(mod_type->export_count, 1);
    ASSERT_NE(mod_type->exports, nullptr);
    EXPECT_STREQ(mod_type->exports[0].name, "test_export");
    EXPECT_EQ(mod_type->exports[0].kind, WASM_EXTERNAL_GLOBAL);
    EXPECT_EQ(mod_type->exports[0].type_idx, 5);
}
