#include "gtest/gtest.h"
#include <vector>
#include <string>
#include <cstring> // For strstr

extern "C" {
#include "wasm_component_loader.h"
// For VALUE_TYPE_xxx and WASM_EXTERNAL_xxx if needed, though not directly for these tests
// #include "wasm_export.h"
}

// Helper: LEB128 encode a uint32_t value
static std::vector<uint8_t>
leb128_u32(uint32_t val)
{
    std::vector<uint8_t> out;
    if (val == 0) {
        out.push_back(0x00);
        return out;
    }
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

// Helper function to construct a minimal component with a canonical section
static std::vector<uint8_t>
build_component_with_canonical_section(
    const std::vector<uint8_t> &canonical_section_payload)
{
    std::vector<uint8_t> component_bytes;
    // Component Header: magic, version, layer
    component_bytes.insert(component_bytes.end(),
                           { 0x00, 0x61, 0x73, 0x6D }); // magic
    component_bytes.insert(component_bytes.end(),
                           { 0x0D, 0x00 }); // version (draft 13 / primary)
    component_bytes.insert(component_bytes.end(), { 0x01, 0x00 }); // layer 1

    // Canonical Section Header
    component_bytes.push_back(COMPONENT_SECTION_ID_CANONICAL); // Section ID 8

    std::vector<uint8_t> size_leb =
        leb128_u32(canonical_section_payload.size());
    component_bytes.insert(component_bytes.end(), size_leb.begin(),
                           size_leb.end()); // Section size

    // Section payload
    component_bytes.insert(component_bytes.end(),
                           canonical_section_payload.begin(),
                           canonical_section_payload.end());
    return component_bytes;
}

class CanonicalSectionParsingTest : public ::testing::Test {
  protected:
    char error_buf[128];
    WASMComponent *component = nullptr;

    void TearDown() override
    {
        if (component) {
            wasm_component_unload(component);
            component = nullptr;
        }
        memset(error_buf, 0, sizeof(error_buf));
    }
};

TEST_F(CanonicalSectionParsingTest, ParseLift)
{
    std::vector<uint8_t> payload;
    payload.push_back(0x01); // Canonical function count: 1
    // Func 0: Lift
    payload.push_back(CANONICAL_FUNC_KIND_LIFT); // kind: 0x00
    payload.push_back(0x00);                     // core_sort_byte: func
    payload.insert(payload.end(), leb128_u32(42).begin(),
                   leb128_u32(42).end()); // core_func_idx: 42
    // Options
    payload.push_back(0x03); // option_count: 3
    payload.push_back(CANONICAL_OPTION_STRING_ENCODING_UTF8); // Opt 0: kind
    payload.push_back(CANONICAL_OPTION_MEMORY_IDX);           // Opt 1: kind
    payload.insert(payload.end(), leb128_u32(0).begin(),
                   leb128_u32(0).end()); // Opt 1: value (mem_idx 0)
    payload.push_back(
        CANONICAL_OPTION_REALLOC_FUNC_IDX); // Opt 2: kind
    payload.insert(payload.end(), leb128_u32(10).begin(),
                   leb128_u32(10).end()); // Opt 2: value (realloc_idx 10)
    // component_func_type_idx after options for LIFT
    payload.insert(payload.end(), leb128_u32(5).begin(),
                   leb128_u32(5).end()); // component_func_type_idx: 5

    std::vector<uint8_t> binary =
        build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->canonical_count, 1);
    const WASMComponentCanonical *canon = &component->canonicals[0];
    EXPECT_EQ(canon->func_kind, CANONICAL_FUNC_KIND_LIFT);
    EXPECT_EQ(canon->u.lift.core_func_idx, 42);
    ASSERT_EQ(canon->option_count, 3);
    ASSERT_NE(canon->options, nullptr);
    EXPECT_EQ(canon->options[0].kind, CANONICAL_OPTION_STRING_ENCODING_UTF8);
    EXPECT_EQ(canon->options[1].kind, CANONICAL_OPTION_MEMORY_IDX);
    EXPECT_EQ(canon->options[1].value, 0);
    EXPECT_EQ(canon->options[2].kind, CANONICAL_OPTION_REALLOC_FUNC_IDX);
    EXPECT_EQ(canon->options[2].value, 10);
    EXPECT_EQ(canon->u.lift.component_func_type_idx, 5);
}

TEST_F(CanonicalSectionParsingTest, ParseLower)
{
    std::vector<uint8_t> payload;
    payload.push_back(0x01); // Canonical function count: 1
    // Func 0: Lower
    payload.push_back(CANONICAL_FUNC_KIND_LOWER); // kind: 0x01
    payload.push_back(0x00);                      // core_sort_byte: func
    payload.insert(payload.end(), leb128_u32(7).begin(),
                   leb128_u32(7).end()); // component_func_idx: 7
    // Options
    payload.push_back(0x01); // option_count: 1
    payload.push_back(
        CANONICAL_OPTION_STRING_ENCODING_UTF16); // Opt 0: kind

    std::vector<uint8_t> binary =
        build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->canonical_count, 1);
    const WASMComponentCanonical *canon = &component->canonicals[0];
    EXPECT_EQ(canon->func_kind, CANONICAL_FUNC_KIND_LOWER);
    EXPECT_EQ(canon->u.lower.component_func_idx, 7);
    ASSERT_EQ(canon->option_count, 1);
    ASSERT_NE(canon->options, nullptr);
    EXPECT_EQ(canon->options[0].kind, CANONICAL_OPTION_STRING_ENCODING_UTF16);
}

TEST_F(CanonicalSectionParsingTest, ParseResourceNew)
{
    std::vector<uint8_t> payload;
    payload.push_back(0x01); // Canonical function count: 1
    // Func 0: ResourceNew
    payload.push_back(CANONICAL_FUNC_KIND_RESOURCE_NEW); // kind: 0x02
    payload.insert(payload.end(), leb128_u32(3).begin(),
                   leb128_u32(3).end()); // resource_type_idx: 3
    // Options
    payload.push_back(0x00); // option_count: 0

    std::vector<uint8_t> binary =
        build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf,
                                    sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->canonical_count, 1);
    const WASMComponentCanonical *canon = &component->canonicals[0];
    EXPECT_EQ(canon->func_kind, CANONICAL_FUNC_KIND_RESOURCE_NEW);
    EXPECT_EQ(canon->u.type_idx_op.type_idx, 3);
    EXPECT_EQ(canon->option_count, 0);
    EXPECT_EQ(canon->options, nullptr);
}

TEST_F(CanonicalSectionParsingTest, ParseResourceDrop)
{
    std::vector<uint8_t> payload = {
        0x01, // count
        CANONICAL_FUNC_KIND_RESOURCE_DROP, // kind
        0x04, // type_idx 4
        0x00  // option_count
    };
    std::vector<uint8_t> binary = build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    const WASMComponentCanonical *canon = &component->canonicals[0];
    EXPECT_EQ(canon->func_kind, CANONICAL_FUNC_KIND_RESOURCE_DROP);
    EXPECT_EQ(canon->u.type_idx_op.type_idx, 4);
    EXPECT_EQ(canon->option_count, 0);
}

TEST_F(CanonicalSectionParsingTest, ParseResourceRep)
{
    std::vector<uint8_t> payload = {
        0x01, // count
        CANONICAL_FUNC_KIND_RESOURCE_REP, // kind
        0x05, // type_idx 5
        0x00  // option_count
    };
    std::vector<uint8_t> binary = build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    const WASMComponentCanonical *canon = &component->canonicals[0];
    EXPECT_EQ(canon->func_kind, CANONICAL_FUNC_KIND_RESOURCE_REP);
    EXPECT_EQ(canon->u.type_idx_op.type_idx, 5);
    EXPECT_EQ(canon->option_count, 0);
}


TEST_F(CanonicalSectionParsingTest, ParseAllNewOptionKinds)
{
    std::vector<uint8_t> payload;
    payload.push_back(0x01); // Canonical function count: 1
    // Func 0: Lift (chosen as it takes options before and a field after)
    payload.push_back(CANONICAL_FUNC_KIND_LIFT); // kind
    payload.push_back(0x00);                     // core_sort_byte
    payload.insert(payload.end(), leb128_u32(1).begin(),
                   leb128_u32(1).end()); // core_func_idx: 1
    // Options
    payload.push_back(0x04); // option_count: 4
    payload.push_back(CANONICAL_OPTION_STRING_ENCODING_LATIN1_UTF16);
    payload.push_back(CANONICAL_OPTION_ASYNC);
    payload.push_back(CANONICAL_OPTION_CALLBACK_FUNC_IDX);
    payload.insert(payload.end(), leb128_u32(99).begin(), leb128_u32(99).end()); // value for callback_idx
    payload.push_back(CANONICAL_OPTION_ALWAYS_TASK_RETURN);

    payload.insert(payload.end(), leb128_u32(2).begin(),
                   leb128_u32(2).end()); // component_func_type_idx: 2

    std::vector<uint8_t> binary = build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->canonical_count, 1);
    const WASMComponentCanonical *canon = &component->canonicals[0];
    ASSERT_EQ(canon->option_count, 4);
    ASSERT_NE(canon->options, nullptr);
    EXPECT_EQ(canon->options[0].kind, CANONICAL_OPTION_STRING_ENCODING_LATIN1_UTF16);
    EXPECT_EQ(canon->options[1].kind, CANONICAL_OPTION_ASYNC);
    EXPECT_EQ(canon->options[2].kind, CANONICAL_OPTION_CALLBACK_FUNC_IDX);
    EXPECT_EQ(canon->options[2].value, 99);
    EXPECT_EQ(canon->options[3].kind, CANONICAL_OPTION_ALWAYS_TASK_RETURN);
}

TEST_F(CanonicalSectionParsingTest, MultipleCanonicalFunctions)
{
    std::vector<uint8_t> payload;
    payload.push_back(0x03); // Canonical function count: 3

    // Func 0: Lift
    payload.push_back(CANONICAL_FUNC_KIND_LIFT); // kind
    payload.push_back(0x00); // core_sort_byte
    payload.insert(payload.end(), leb128_u32(10).begin(), leb128_u32(10).end()); // core_func_idx
    payload.push_back(0x00); // option_count
    payload.insert(payload.end(), leb128_u32(1).begin(), leb128_u32(1).end()); // component_func_type_idx

    // Func 1: Lower
    payload.push_back(CANONICAL_FUNC_KIND_LOWER); // kind
    payload.push_back(0x00); // core_sort_byte
    payload.insert(payload.end(), leb128_u32(20).begin(), leb128_u32(20).end()); // component_func_idx
    payload.push_back(0x00); // option_count

    // Func 2: ResourceNew
    payload.push_back(CANONICAL_FUNC_KIND_RESOURCE_NEW); // kind
    payload.insert(payload.end(), leb128_u32(30).begin(), leb128_u32(30).end()); // type_idx
    payload.push_back(0x00); // option_count

    std::vector<uint8_t> binary = build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;

    ASSERT_EQ(component->canonical_count, 3);
    // Canon 0 (Lift)
    EXPECT_EQ(component->canonicals[0].func_kind, CANONICAL_FUNC_KIND_LIFT);
    EXPECT_EQ(component->canonicals[0].u.lift.core_func_idx, 10);
    EXPECT_EQ(component->canonicals[0].u.lift.component_func_type_idx, 1);
    EXPECT_EQ(component->canonicals[0].option_count, 0);
    // Canon 1 (Lower)
    EXPECT_EQ(component->canonicals[1].func_kind, CANONICAL_FUNC_KIND_LOWER);
    EXPECT_EQ(component->canonicals[1].u.lower.component_func_idx, 20);
    EXPECT_EQ(component->canonicals[1].option_count, 0);
    // Canon 2 (ResourceNew)
    EXPECT_EQ(component->canonicals[2].func_kind, CANONICAL_FUNC_KIND_RESOURCE_NEW);
    EXPECT_EQ(component->canonicals[2].u.type_idx_op.type_idx, 30);
    EXPECT_EQ(component->canonicals[2].option_count, 0);
}

TEST_F(CanonicalSectionParsingTest, ParseYieldAsync)
{
    std::vector<uint8_t> payload = {
        0x01, // count
        CANONICAL_FUNC_KIND_YIELD, // kind (0x0C)
        0x01, // async_opt_byte: 0x01 (true)
        0x00  // option_count
    };
    std::vector<uint8_t> binary = build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    const WASMComponentCanonical *canon = &component->canonicals[0];
    EXPECT_EQ(canon->func_kind, CANONICAL_FUNC_KIND_YIELD);
    // The async_opt_byte is logged but not stored in the union in current impl.
    // Test primarily ensures parsing succeeds and correct kind is set.
    EXPECT_EQ(canon->option_count, 0);
}

TEST_F(CanonicalSectionParsingTest, ParseWaitableSetWait)
{
    std::vector<uint8_t> payload = {
        0x01, // count
        CANONICAL_FUNC_KIND_WAITABLE_SET_WAIT, // kind (0x20)
        0x00, // async_opt_byte: 0x00 (false)
        0x01, // mem_idx: 1
        0x00  // option_count
    };
    std::vector<uint8_t> binary = build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_NE(component, nullptr) << error_buf;
    ASSERT_EQ(component->canonical_count, 1);
    const WASMComponentCanonical *canon = &component->canonicals[0];
    EXPECT_EQ(canon->func_kind, CANONICAL_FUNC_KIND_WAITABLE_SET_WAIT);
    EXPECT_EQ(canon->u.waitable_mem_op.async_opt, 0x00);
    EXPECT_EQ(canon->u.waitable_mem_op.mem_idx, 1);
    EXPECT_EQ(canon->option_count, 0);
}

TEST_F(CanonicalSectionParsingTest, InvalidCanonicalFuncKind) {
    const std::vector<uint8_t> canonical_section_bytes = {
        // Section ID 8 (Canonical)
        // Size will be calculated
        // Payload:
        0x01, // Canonical function count: 1
        0xFF, // func_kind: INVALID (0xFF, assuming this is not a valid kind)
        // Minimal data to make it seem like a function, though it should fail on kind
        0x00, // sort_byte for lift/lower (parser might not reach here)
        0x0A, // core_func_idx: 10
        0x00, // options_count: 0
        0x05  // component_func_type_idx: 5
    };
    // Build component with only the canonical section payload
    std::vector<uint8_t> full_binary = build_component_with_canonical_section(canonical_section_bytes);

    component = wasm_component_load(full_binary.data(), full_binary.size(), error_buf, sizeof(error_buf));
    ASSERT_EQ(component, nullptr);
    EXPECT_NE(strstr(error_buf, "unknown or unsupported canonical func kind"), nullptr) << error_buf;
}

TEST_F(CanonicalSectionParsingTest, InvalidOptionKind) {
    const std::vector<uint8_t> payload = {
        0x01, // Canonical function count: 1
        CANONICAL_FUNC_KIND_LIFT, // kind: 0x00
        0x00, // core_sort_byte: func
        0x01, // core_func_idx: 1
        0x01, // option_count: 1
        0xFF, // Invalid option kind
        0x01  // component_func_type_idx: 1
    };
     std::vector<uint8_t> binary = build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_EQ(component, nullptr);
    EXPECT_NE(strstr(error_buf, "unknown canonical option kind"), nullptr) << error_buf;
}

TEST_F(CanonicalSectionParsingTest, InvalidLiftMissingSortByte) {
    // Missing the 0x00 sort byte after LIFT kind
    std::vector<uint8_t> payload = {
        0x01, // Canonical function count: 1
        CANONICAL_FUNC_KIND_LIFT, // kind: 0x00
        // Missing 0x00 sort byte here
        0x2A, // core_func_idx: 42 (will be misparsed as sort byte)
        0x00, // option_count: 0
        0x05  // component_func_type_idx: 5
    };
    std::vector<uint8_t> binary = build_component_with_canonical_section(payload);
    component = wasm_component_load(binary.data(), binary.size(), error_buf, sizeof(error_buf));
    ASSERT_EQ(component, nullptr);
    EXPECT_NE(strstr(error_buf, "unexpected sort byte"), nullptr) << error_buf;
}
