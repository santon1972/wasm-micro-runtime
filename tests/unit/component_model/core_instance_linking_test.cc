#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <utility> // For std::pair

extern "C" {
#include "wasm_component_loader.h"
#include "wasm_export.h" // For VALUE_TYPE_xxx and WASM_EXTERNAL_xxx
// CORE_TYPE_KIND_MODULE is defined in wasm_component_loader.h
}

// Helper: LEB128 encode a uint32_t value
static std::vector<uint8_t>
leb128_u32(uint32_t val)
{
    std::vector<uint8_t> out;
    do {
        uint8_t byte = val & 0x7F;
        val >>= 7;
        if (val != 0) {
            byte |= 0x80;
        }
        out.push_back(byte);
    } while (val != 0);
    return out;
}

// Helper: Create a string payload (LEB128 size + string)
static std::vector<uint8_t>
string_to_payload(const std::string &s)
{
    std::vector<uint8_t> payload;
    std::vector<uint8_t> s_len_leb = leb128_u32(s.length());
    payload.insert(payload.end(), s_len_leb.begin(), s_len_leb.end());
    payload.insert(payload.end(), s.begin(), s.end());
    return payload;
}

// Helper function to construct a component from multiple raw sections
static std::vector<uint8_t>
build_component_from_sections(
    const std::vector<std::pair<uint8_t, std::vector<uint8_t>>>
        &sections)
{
    std::vector<uint8_t> component_bytes;
    // Component Header: magic, version, layer
    component_bytes.insert(component_bytes.end(),
                           { 0x00, 0x61, 0x73, 0x6D }); // magic
    component_bytes.insert(component_bytes.end(),
                           { 0x0D, 0x00 }); // version (draft 13)
    component_bytes.insert(component_bytes.end(), { 0x01, 0x00 }); // layer 1

    for (const auto &section_pair : sections) {
        component_bytes.push_back(section_pair.first); // Section ID
        std::vector<uint8_t> size_leb =
            leb128_u32(section_pair.second.size());
        component_bytes.insert(component_bytes.end(), size_leb.begin(),
                               size_leb.end()); // Section size
        component_bytes.insert(component_bytes.end(),
                               section_pair.second.begin(),
                               section_pair.second.end()); // Section payload
    }
    return component_bytes;
}

class CoreInstanceLinkingTest : public ::testing::Test {
  protected:
    char error_buf[256]; // Increased size for potentially longer error messages
    WASMComponent *component = nullptr;

    void TearDown() override
    {
        if (component) {
            wasm_component_unload(component);
            component = nullptr;
        }
        memset(error_buf, 0, sizeof(error_buf));
    }

    // Consumer Module (Core Module 1 in Test Case 1)
    // Imports: "env"."imp_func" (func, type_idx 0), "env"."imp_global" (global, i32 const)
    // Defines 1 func type: ()->() at index 0
    std::vector<uint8_t> build_consumer_module_bytecode()
    {
        std::vector<uint8_t> module_bytecode;
        // Magic & Version
        module_bytecode.insert(module_bytecode.end(),
                               { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 });

        // Type Section (ID 1) - defines func type 0: () -> ()
        std::vector<uint8_t> type_section_payload = {
            0x01, // type count
            0x60, // func type
            0x00, // param count
            0x00  // result count
        };
        module_bytecode.push_back(0x01); // Section ID
        module_bytecode.insert(module_bytecode.end(),
                               leb128_u32(type_section_payload.size()).begin(),
                               leb128_u32(type_section_payload.size()).end());
        module_bytecode.insert(module_bytecode.end(),
                               type_section_payload.begin(),
                               type_section_payload.end());

        // Import Section (ID 2)
        std::vector<uint8_t> import_section_payload;
        import_section_payload.push_back(0x02); // import count

        // Import 1: "env"."imp_func" (func, type_idx 0)
        std::vector<uint8_t> mod_name_env = string_to_payload("env");
        std::vector<uint8_t> field_name_imp_func = string_to_payload("imp_func");
        import_section_payload.insert(import_section_payload.end(), mod_name_env.begin(), mod_name_env.end());
        import_section_payload.insert(import_section_payload.end(), field_name_imp_func.begin(), field_name_imp_func.end());
        import_section_payload.push_back(WASM_EXTERNAL_FUNCTION); // kind: func
        import_section_payload.push_back(0x00);                   // type_idx: 0

        // Import 2: "env"."imp_global" (global, i32 const)
        std::vector<uint8_t> field_name_imp_global = string_to_payload("imp_global");
        import_section_payload.insert(import_section_payload.end(), mod_name_env.begin(), mod_name_env.end());
        import_section_payload.insert(import_section_payload.end(), field_name_imp_global.begin(), field_name_imp_global.end());
        import_section_payload.push_back(WASM_EXTERNAL_GLOBAL); // kind: global
        import_section_payload.push_back(VALUE_TYPE_I32);       // type: i32
        import_section_payload.push_back(0x00);                 // mutability: const

        module_bytecode.push_back(0x02); // Section ID
        module_bytecode.insert(module_bytecode.end(),
                               leb128_u32(import_section_payload.size()).begin(),
                               leb128_u32(import_section_payload.size()).end());
        module_bytecode.insert(module_bytecode.end(),
                               import_section_payload.begin(),
                               import_section_payload.end());
        return module_bytecode;
    }
};

TEST_F(CoreInstanceLinkingTest, ValidLinking)
{
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sections;

    // Section 0: Core Module Section (defines 1 module - the consumer)
    std::vector<uint8_t> consumer_module_bytes = build_consumer_module_bytecode();
    std::vector<uint8_t> core_module_section_payload;
    core_module_section_payload.push_back(0x01); // module count
    core_module_section_payload.insert(core_module_section_payload.end(),
                                       leb128_u32(consumer_module_bytes.size()).begin(),
                                       leb128_u32(consumer_module_bytes.size()).end());
    core_module_section_payload.insert(core_module_section_payload.end(),
                                       consumer_module_bytes.begin(),
                                       consumer_module_bytes.end());
    sections.push_back({ COMPONENT_SECTION_ID_CORE_MODULE, core_module_section_payload });

    // Section 1: Core Instance Section
    std::vector<uint8_t> core_instance_section_payload;
    core_instance_section_payload.push_back(0x02); // instance count: 2

    // Instance 0 (Provider - Inline Exports)
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INLINE_EXPORT); // kind: inline export
    core_instance_section_payload.push_back(0x02); // export count: 2
        // Export 1: "exp_func", func, sort_idx 0
        std::vector<uint8_t> exp_func_name = string_to_payload("exp_func");
        core_instance_section_payload.insert(core_instance_section_payload.end(), exp_func_name.begin(), exp_func_name.end());
        core_instance_section_payload.push_back(WASM_EXTERNAL_FUNCTION);
        core_instance_section_payload.push_back(0x00); // sort_idx 0 (dummy)
        // Export 2: "exp_global", global, sort_idx 0
        std::vector<uint8_t> exp_global_name = string_to_payload("exp_global");
        core_instance_section_payload.insert(core_instance_section_payload.end(), exp_global_name.begin(), exp_global_name.end());
        core_instance_section_payload.push_back(WASM_EXTERNAL_GLOBAL);
        core_instance_section_payload.push_back(0x00); // sort_idx 0 (dummy)

    // Instance 1 (Consumer - Instantiates Module 0)
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INSTANTIATE); // kind: instantiate
    core_instance_section_payload.push_back(0x00); // module_idx: 0 (consumer module)
    core_instance_section_payload.push_back(0x02); // arg count: 2
        // Arg 1: name "imp_func", instance_idx 0 (provider instance)
        std::vector<uint8_t> arg_imp_func_name = string_to_payload("imp_func");
        core_instance_section_payload.insert(core_instance_section_payload.end(), arg_imp_func_name.begin(), arg_imp_func_name.end());
        core_instance_section_payload.push_back(0x00); // instance_idx: 0
        // Arg 2: name "imp_global", instance_idx 0 (provider instance)
        std::vector<uint8_t> arg_imp_global_name = string_to_payload("imp_global");
        core_instance_section_payload.insert(core_instance_section_payload.end(), arg_imp_global_name.begin(), arg_imp_global_name.end());
        core_instance_section_payload.push_back(0x00); // instance_idx: 0
    sections.push_back({ COMPONENT_SECTION_ID_CORE_INSTANCE, core_instance_section_payload });

    std::vector<uint8_t> binary = build_component_from_sections(sections);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << "Load failed: " << error_buf;

    ASSERT_EQ(component->core_instance_count, 2);
    WASMComponentCoreInstance* consumer_inst = &component->core_instances[1];
    ASSERT_EQ(consumer_inst->kind, CORE_INSTANCE_KIND_INSTANTIATE);
    ASSERT_EQ(consumer_inst->u.instantiate.arg_count, 2);

    WASMComponentCoreInstanceArg* arg0 = &consumer_inst->u.instantiate.args[0];
    EXPECT_STREQ(arg0->name, "imp_func");
    EXPECT_EQ(arg0->kind, WASM_EXTERNAL_FUNCTION); // Validated by kind derivation + linking validation

    WASMComponentCoreInstanceArg* arg1 = &consumer_inst->u.instantiate.args[1];
    EXPECT_STREQ(arg1->name, "imp_global");
    EXPECT_EQ(arg1->kind, WASM_EXTERNAL_GLOBAL); // Validated
}

TEST_F(CoreInstanceLinkingTest, InvalidLinkExportNameMismatch)
{
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sections;

    // Section 0: Core Module Section (Consumer)
    std::vector<uint8_t> consumer_module_bytes = build_consumer_module_bytecode();
    std::vector<uint8_t> core_module_section_payload;
    core_module_section_payload.push_back(0x01); // module count
    core_module_section_payload.insert(core_module_section_payload.end(), leb128_u32(consumer_module_bytes.size()).begin(), leb128_u32(consumer_module_bytes.size()).end());
    core_module_section_payload.insert(core_module_section_payload.end(), consumer_module_bytes.begin(), consumer_module_bytes.end());
    sections.push_back({ COMPONENT_SECTION_ID_CORE_MODULE, core_module_section_payload });

    // Section 1: Core Instance Section
    std::vector<uint8_t> core_instance_section_payload;
    core_instance_section_payload.push_back(0x02); // instance count

    // Instance 0 (Provider - Inline Exports) - exports "wrong_exp_func"
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INLINE_EXPORT);
    core_instance_section_payload.push_back(0x01); // export count
        std::vector<uint8_t> wrong_exp_func_name = string_to_payload("wrong_exp_func"); // Different name
        core_instance_section_payload.insert(core_instance_section_payload.end(), wrong_exp_func_name.begin(), wrong_exp_func_name.end());
        core_instance_section_payload.push_back(WASM_EXTERNAL_FUNCTION);
        core_instance_section_payload.push_back(0x00); // sort_idx

    // Instance 1 (Consumer - Instantiates Module 0) - tries to import "imp_func"
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INSTANTIATE);
    core_instance_section_payload.push_back(0x00); // module_idx
    core_instance_section_payload.push_back(0x01); // arg count
        std::vector<uint8_t> arg_imp_func_name = string_to_payload("imp_func");
        core_instance_section_payload.insert(core_instance_section_payload.end(), arg_imp_func_name.begin(), arg_imp_func_name.end());
        core_instance_section_payload.push_back(0x00); // instance_idx

    sections.push_back({ COMPONENT_SECTION_ID_CORE_INSTANCE, core_instance_section_payload });

    std::vector<uint8_t> binary = build_component_from_sections(sections);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_EQ(component, nullptr);
    EXPECT_NE(std::string(error_buf).find("Required export 'imp_func' of kind 0 not found in source core instance 0"), std::string::npos)
        << "Error message was: " << error_buf;
}

TEST_F(CoreInstanceLinkingTest, InvalidLinkExportKindMismatch)
{
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sections;
    // Consumer module (Module 0) imports "imp_func" as FUNCTION
    std::vector<uint8_t> consumer_module_bytes = build_consumer_module_bytecode();
    std::vector<uint8_t> core_module_section_payload;
    core_module_section_payload.push_back(0x01); // module count
    core_module_section_payload.insert(core_module_section_payload.end(), leb128_u32(consumer_module_bytes.size()).begin(), leb128_u32(consumer_module_bytes.size()).end());
    core_module_section_payload.insert(core_module_section_payload.end(), consumer_module_bytes.begin(), consumer_module_bytes.end());
    sections.push_back({ COMPONENT_SECTION_ID_CORE_MODULE, core_module_section_payload });

    std::vector<uint8_t> core_instance_section_payload;
    core_instance_section_payload.push_back(0x02); // instance count

    // Instance 0 (Provider) exports "exp_func" (same name as "imp_func" effectively via arg name) but as GLOBAL
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INLINE_EXPORT);
    core_instance_section_payload.push_back(0x01); // export count
        std::vector<uint8_t> exp_func_name = string_to_payload("imp_func"); // Provider exports with name "imp_func"
        core_instance_section_payload.insert(core_instance_section_payload.end(), exp_func_name.begin(), exp_func_name.end());
        core_instance_section_payload.push_back(WASM_EXTERNAL_GLOBAL); // Exported as GLOBAL
        core_instance_section_payload.push_back(0x00); // sort_idx

    // Instance 1 (Consumer) instantiates Module 0, trying to link its "imp_func" (which is FUNCTION)
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INSTANTIATE);
    core_instance_section_payload.push_back(0x00); // module_idx
    core_instance_section_payload.push_back(0x01); // arg count
        std::vector<uint8_t> arg_imp_func_name = string_to_payload("imp_func");
        core_instance_section_payload.insert(core_instance_section_payload.end(), arg_imp_func_name.begin(), arg_imp_func_name.end());
        core_instance_section_payload.push_back(0x00); // instance_idx

    sections.push_back({ COMPONENT_SECTION_ID_CORE_INSTANCE, core_instance_section_payload });
    std::vector<uint8_t> binary = build_component_from_sections(sections);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_EQ(component, nullptr);
    // Kind derivation for "imp_func" in consumer module results in WASM_EXTERNAL_FUNCTION (0)
    // Provider exports "imp_func" as WASM_EXTERNAL_GLOBAL (3)
    // So, the error should be "Required export 'imp_func' of kind 0 not found..."
    EXPECT_NE(std::string(error_buf).find("Required export 'imp_func' of kind 0 not found in source core instance 0"), std::string::npos)
        << "Error message was: " << error_buf;
}

TEST_F(CoreInstanceLinkingTest, InvalidLinkSourceNotInlineExport)
{
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sections;
    // Consumer module (Module 0)
    std::vector<uint8_t> consumer_module_bytes = build_consumer_module_bytecode();
    std::vector<uint8_t> core_module_section_payload;
    core_module_section_payload.push_back(0x02); // module count: 2 (consumer and a dummy for source instance)
    // Module 0: Consumer
    core_module_section_payload.insert(core_module_section_payload.end(), leb128_u32(consumer_module_bytes.size()).begin(), leb128_u32(consumer_module_bytes.size()).end());
    core_module_section_payload.insert(core_module_section_payload.end(), consumer_module_bytes.begin(), consumer_module_bytes.end());
    // Module 1: Dummy module for source instance
    std::vector<uint8_t> dummy_module_bytes = { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 }; // Minimal valid module
    core_module_section_payload.insert(core_module_section_payload.end(), leb128_u32(dummy_module_bytes.size()).begin(), leb128_u32(dummy_module_bytes.size()).end());
    core_module_section_payload.insert(core_module_section_payload.end(), dummy_module_bytes.begin(), dummy_module_bytes.end());
    sections.push_back({ COMPONENT_SECTION_ID_CORE_MODULE, core_module_section_payload });

    std::vector<uint8_t> core_instance_section_payload;
    core_instance_section_payload.push_back(0x02); // instance count: 2

    // Instance 0 (Source - Instantiate, not Inline Export)
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INSTANTIATE);
    core_instance_section_payload.push_back(0x01); // module_idx 1 (dummy module)
    core_instance_section_payload.push_back(0x00); // arg count 0

    // Instance 1 (Consumer - Instantiates Module 0)
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INSTANTIATE);
    core_instance_section_payload.push_back(0x00); // module_idx 0 (consumer module)
    core_instance_section_payload.push_back(0x01); // arg count
        std::vector<uint8_t> arg_imp_func_name = string_to_payload("imp_func");
        core_instance_section_payload.insert(core_instance_section_payload.end(), arg_imp_func_name.begin(), arg_imp_func_name.end());
        core_instance_section_payload.push_back(0x00); // instance_idx 0 (points to the instantiate-kind instance)

    sections.push_back({ COMPONENT_SECTION_ID_CORE_INSTANCE, core_instance_section_payload });
    std::vector<uint8_t> binary = build_component_from_sections(sections);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_EQ(component, nullptr);
    EXPECT_NE(std::string(error_buf).find("Source core instance 0 for argument 'imp_func' is not an inline export group"), std::string::npos)
        << "Error message was: " << error_buf;
}

TEST_F(CoreInstanceLinkingTest, InvalidLinkSourceIndexOutOfBounds)
{
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sections;
    std::vector<uint8_t> consumer_module_bytes = build_consumer_module_bytecode();
    std::vector<uint8_t> core_module_section_payload;
    core_module_section_payload.push_back(0x01); // module count
    core_module_section_payload.insert(core_module_section_payload.end(), leb128_u32(consumer_module_bytes.size()).begin(), leb128_u32(consumer_module_bytes.size()).end());
    core_module_section_payload.insert(core_module_section_payload.end(), consumer_module_bytes.begin(), consumer_module_bytes.end());
    sections.push_back({ COMPONENT_SECTION_ID_CORE_MODULE, core_module_section_payload });

    std::vector<uint8_t> core_instance_section_payload;
    core_instance_section_payload.push_back(0x01); // instance count: 1 (only consumer)

    // Instance 0 (Consumer - Instantiates Module 0)
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INSTANTIATE);
    core_instance_section_payload.push_back(0x00); // module_idx
    core_instance_section_payload.push_back(0x01); // arg count
        std::vector<uint8_t> arg_imp_func_name = string_to_payload("imp_func");
        core_instance_section_payload.insert(core_instance_section_payload.end(), arg_imp_func_name.begin(), arg_imp_func_name.end());
        core_instance_section_payload.push_back(0x63); // instance_idx 99 (0x63) - out of bounds

    sections.push_back({ COMPONENT_SECTION_ID_CORE_INSTANCE, core_instance_section_payload });
    std::vector<uint8_t> binary = build_component_from_sections(sections);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_EQ(component, nullptr);
    EXPECT_NE(std::string(error_buf).find("source core instance index 99 out of bounds for argument 'imp_func'"), std::string::npos)
        << "Error message was: " << error_buf;
}

TEST_F(CoreInstanceLinkingTest, InvalidLinkImportNameNotFoundForKindDerivation)
{
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> sections;

    // Section 0: Core Module Section (Consumer - Module 0)
    // This consumer module *does not* import "imp_func_non_existent"
    std::vector<uint8_t> consumer_module_bytes = build_consumer_module_bytecode(); // This one imports "imp_func" and "imp_global"
    // We need a consumer module that *doesn't* import the name we're trying to use.
    // Let's build a consumer module that imports nothing for this test.
    std::vector<uint8_t> empty_consumer_module;
    empty_consumer_module.insert(empty_consumer_module.end(), { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 });
    // No Type or Import section.

    std::vector<uint8_t> core_module_section_payload;
    core_module_section_payload.push_back(0x01); // module count
    core_module_section_payload.insert(core_module_section_payload.end(), leb128_u32(empty_consumer_module.size()).begin(), leb128_u32(empty_consumer_module.size()).end());
    core_module_section_payload.insert(core_module_section_payload.end(), empty_consumer_module.begin(), empty_consumer_module.end());
    sections.push_back({ COMPONENT_SECTION_ID_CORE_MODULE, core_module_section_payload });

    // Section 1: Core Instance Section
    std::vector<uint8_t> core_instance_section_payload;
    core_instance_section_payload.push_back(0x02); // instance count: 2

    // Instance 0 (Provider - Inline Exports) - exports "exp_func"
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INLINE_EXPORT);
    core_instance_section_payload.push_back(0x01); // export count
        std::vector<uint8_t> exp_func_name = string_to_payload("exp_func");
        core_instance_section_payload.insert(core_instance_section_payload.end(), exp_func_name.begin(), exp_func_name.end());
        core_instance_section_payload.push_back(WASM_EXTERNAL_FUNCTION);
        core_instance_section_payload.push_back(0x00); // sort_idx

    // Instance 1 (Consumer - Instantiates Module 0) - arg "imp_func_non_existent"
    core_instance_section_payload.push_back(CORE_INSTANCE_KIND_INSTANTIATE);
    core_instance_section_payload.push_back(0x00); // module_idx 0 (empty_consumer_module)
    core_instance_section_payload.push_back(0x01); // arg count
        std::vector<uint8_t> arg_name = string_to_payload("imp_func_non_existent");
        core_instance_section_payload.insert(core_instance_section_payload.end(), arg_name.begin(), arg_name.end());
        core_instance_section_payload.push_back(0x00); // instance_idx 0 (provider)

    sections.push_back({ COMPONENT_SECTION_ID_CORE_INSTANCE, core_instance_section_payload });

    std::vector<uint8_t> binary = build_component_from_sections(sections);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_EQ(component, nullptr);
    EXPECT_NE(std::string(error_buf).find("import 'imp_func_non_existent' not found in target module 0 for kind derivation"), std::string::npos)
        << "Error message was: " << error_buf;
}
