#include "gtest/gtest.h"

extern "C" {
#include "wasm_component_loader.h" // Main entry point for parsing
#include "wasm_component.h"      // For WASMComponent struct and enums
// For wasm_component_unload, if not pulled in by wasm_component_loader.h
// Might need wasm_runtime_free from wasm_c_api_internal.h if not transitive
}

#include <vector>
#include <cstring> // For memcpy

// Helper function to construct a minimal component for testing a specific section
static std::vector<uint8_t> create_component_bytes(const std::vector<uint8_t>& section_bytes) {
    std::vector<uint8_t> component_header = {
        0x00, 0x61, 0x73, 0x6d, // WASM_MAGIC_NUMBER
        0x0d, 0x00, 0x00, 0x00, // COMPONENT_MODEL_VERSION_PRIMARY (assuming 0x0d)
        0x01, 0x00              // COMPONENT_MODEL_LAYER_PRIMARY (assuming 0x01)
    };
    
    std::vector<uint8_t> full_component;
    full_component.insert(full_component.end(), component_header.begin(), component_header.end());
    full_component.insert(full_component.end(), section_bytes.begin(), section_bytes.end());
    return full_component;
}

class CanonicalSectionTest : public ::testing::Test {
protected:
    WASMComponent* component = nullptr;
    char error_buf[128] = {0};

    void TearDown() override {
        if (component) {
            wasm_component_unload(component);
            component = nullptr;
        }
    }

    void LoadComponent(const std::vector<uint8_t>& section_bytes) {
        std::vector<uint8_t> component_data = create_component_bytes(section_bytes);
        component = wasm_component_load(component_data.data(), component_data.size(), error_buf, sizeof(error_buf));
    }
};


TEST_F(CanonicalSectionTest, ParseLiftMinimal) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, // Section ID
        0x05, // Section Size
        0x01, // Canonical function count
        0x00, // func_kind: canon_lift
        0x0A, // core_func_idx: 10
        0x05, // component_func_type_idx: 5
        0x00  // options_count: 0
    };
    LoadComponent(canonical_section_bytes);

    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    if (component->canonical_count > 0) {
        const WASMComponentCanonical& canonical = component->canonicals[0];
        EXPECT_EQ(canonical.func_kind, (uint8_t)WASM_CANONICAL_FUNCTION_LIFT);
        EXPECT_EQ(canonical.core_func_idx, 10);
        EXPECT_EQ(canonical.component_func_type_idx, 5);
        EXPECT_EQ(canonical.options.string_encoding, CANONICAL_STRING_ENCODING_UTF8); // Default
        EXPECT_EQ(canonical.options.memory_idx, (uint32_t)-1); // Default
        EXPECT_EQ(canonical.options.realloc_idx, (uint32_t)-1); // Default
        EXPECT_EQ(canonical.options.post_return_idx, (uint32_t)-1); // Default
    }
}

TEST_F(CanonicalSectionTest, ParseLowerMinimal) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, // Section ID
        0x05, // Section Size
        0x01, // Canonical function count
        0x01, // func_kind: canon_lower
        0x07, // core_func_idx: 7
        0x03, // component_func_type_idx: 3
        0x00  // options_count: 0
    };
    LoadComponent(canonical_section_bytes);

    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    if (component->canonical_count > 0) {
        const WASMComponentCanonical& canonical = component->canonicals[0];
        EXPECT_EQ(canonical.func_kind, (uint8_t)WASM_CANONICAL_FUNCTION_LOWER);
        EXPECT_EQ(canonical.core_func_idx, 7);
        EXPECT_EQ(canonical.component_func_type_idx, 3);
    }
}

TEST_F(CanonicalSectionTest, ParseLiftUTF8) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, // Section ID
        0x07, // Section Size
        0x01, // Canonical function count
        0x00, // func_kind: canon_lift
        0x0A, // core_func_idx: 10
        0x05, // component_func_type_idx: 5
        0x01, // options_count: 1
        0x00, // option_kind: string_encoding
        0x00  // option_value: utf8
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    EXPECT_EQ(component->canonicals[0].options.string_encoding, CANONICAL_STRING_ENCODING_UTF8);
}

TEST_F(CanonicalSectionTest, ParseLiftUTF16) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, 0x07, 0x01, 0x00, 0x0B, 0x06, 0x01, 0x00, 0x01
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    EXPECT_EQ(component->canonicals[0].options.string_encoding, CANONICAL_STRING_ENCODING_UTF16);
}

TEST_F(CanonicalSectionTest, ParseLiftCompactUTF16) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, 0x07, 0x01, 0x00, 0x0C, 0x07, 0x01, 0x00, 0x02
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    EXPECT_EQ(component->canonicals[0].options.string_encoding, CANONICAL_STRING_ENCODING_COMPACT_UTF16);
}

TEST_F(CanonicalSectionTest, ParseLiftWithMemory) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, // Section ID
        0x07, // Section Size
        0x01, // Count
        0x00, // Lift
        0x01, // Core func idx
        0x01, // Comp func type idx
        0x01, // Options count
        0x01, // Memory option kind
        0x0F  // Memory Idx 15
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    EXPECT_EQ(component->canonicals[0].options.memory_idx, 15);
}

TEST_F(CanonicalSectionTest, ParseLiftWithRealloc) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, 0x07, 0x01, 0x00, 0x02, 0x02, 0x01, 0x02, 0xAA // Realloc Idx 170 (0xAA)
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    EXPECT_EQ(component->canonicals[0].options.realloc_idx, 0xAA);
}

TEST_F(CanonicalSectionTest, ParseLiftWithPostReturn) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, 0x07, 0x01, 0x00, 0x03, 0x03, 0x01, 0x03, 0xBB // PostReturn Idx 187 (0xBB)
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    EXPECT_EQ(component->canonicals[0].options.post_return_idx, 0xBB);
}

TEST_F(CanonicalSectionTest, ParseLiftWithAllOptions) {
    // Lift, core_func=4, comp_type=4
    // Options: string_encoding=utf16 (0x01), memory_idx=1, realloc_idx=2, post_return_idx=3
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, // Section ID
        0x0D, // Section Size
        0x01, // Count
        0x00, // Lift
        0x04, // Core func idx
        0x04, // Comp func type idx
        0x04, // Options count
        0x00, 0x01, // string_encoding utf16
        0x01, 0x01, // memory_idx 1
        0x02, 0x02, // realloc_idx 2
        0x03, 0x03  // post_return_idx 3
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    const auto& opts = component->canonicals[0].options;
    EXPECT_EQ(opts.string_encoding, CANONICAL_STRING_ENCODING_UTF16);
    EXPECT_EQ(opts.memory_idx, 1);
    EXPECT_EQ(opts.realloc_idx, 2);
    EXPECT_EQ(opts.post_return_idx, 3);
}

TEST_F(CanonicalSectionTest, ParseMultipleCanonicals) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, // Section ID
        0x0E, // Section Size
        0x02, // Canonical function count
        // Entry 1
        0x00, // func_kind: canon_lift
        0x0A, // core_func_idx: 10
        0x05, // component_func_type_idx: 5
        0x01, // options_count: 1
        0x00, 0x00, // string_encoding: utf8
        // Entry 2
        0x01, // func_kind: canon_lower
        0x07, // core_func_idx: 7
        0x03, // component_func_type_idx: 3
        0x00  // options_count: 0
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_NE(component, nullptr) << "wasm_component_load failed: " << error_buf;
    ASSERT_EQ(component->canonical_count, 2);
    
    const WASMComponentCanonical& c1 = component->canonicals[0];
    EXPECT_EQ(c1.func_kind, (uint8_t)WASM_CANONICAL_FUNCTION_LIFT);
    EXPECT_EQ(c1.core_func_idx, 10);
    EXPECT_EQ(c1.component_func_type_idx, 5);
    EXPECT_EQ(c1.options.string_encoding, CANONICAL_STRING_ENCODING_UTF8);

    const WASMComponentCanonical& c2 = component->canonicals[1];
    EXPECT_EQ(c2.func_kind, (uint8_t)WASM_CANONICAL_FUNCTION_LOWER);
    EXPECT_EQ(c2.core_func_idx, 7);
    EXPECT_EQ(c2.component_func_type_idx, 3);
    EXPECT_EQ(c2.options.string_encoding, CANONICAL_STRING_ENCODING_UTF8); // Default
}

TEST_F(CanonicalSectionTest, InvalidStringEncoding) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, 0x07, 0x01, 0x00, 0x0A, 0x05, 0x01, 0x00, 0x03 // Invalid encoding 0x03
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_EQ(component, nullptr); // Expect parsing to fail
    EXPECT_NE(strstr(error_buf, "invalid string encoding"), nullptr);
}

TEST_F(CanonicalSectionTest, UnknownOptionKind) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, 0x07, 0x01, 0x00, 0x0A, 0x05, 0x01, 0x04, 0x00 // Unknown option kind 0x04
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_EQ(component, nullptr); // Expect parsing to fail
    EXPECT_NE(strstr(error_buf, "unknown canonical option kind"), nullptr);
}

TEST_F(CanonicalSectionTest, InvalidCanonicalFuncKind) {
    const std::vector<uint8_t> canonical_section_bytes = {
        0x08, // Section ID
        0x05, // Section Size
        0x01, // Canonical function count
        0x02, // func_kind: INVALID (0x02)
        0x0A, // core_func_idx: 10
        0x05, // component_func_type_idx: 5
        0x00  // options_count: 0
    };
    LoadComponent(canonical_section_bytes);
    ASSERT_EQ(component, nullptr);
    EXPECT_NE(strstr(error_buf, "invalid canonical function kind"), nullptr);
}
