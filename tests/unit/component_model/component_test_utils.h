#ifndef _COMPONENT_TEST_UTILS_H
#define _COMPONENT_TEST_UTILS_H

#include "gtest/gtest.h"
#include "wasm_export.h"       // For WASMModuleInstance, etc.
#include "wasm_c_api.h"        // If using C API style tests
#include "wasm_component_loader.h"
#include "wasm_component_runtime.h"
#include "wasm_runtime.h"      // For wasm_runtime_load, wasm_runtime_instantiate, etc.
#include "bh_read_file.h"    // For bh_read_file_to_buffer

#include <vector>
#include <string>

// Helper to load a WASM core module from a .wasm file
inline WASMModuleCommon* load_wasm_module(const char* file_path, char* error_buf, uint32_t error_buf_size) {
    uint32_t size;
    uint8 *buffer = (uint8*)bh_read_file_to_buffer(file_path, &size);
    if (!buffer) {
        snprintf(error_buf, error_buf_size, "Failed to read wasm file %s", file_path);
        return NULL;
    }

    WASMModuleCommon *module = wasm_runtime_load(buffer, size, error_buf, error_buf_size);
    bh_free(buffer);
    return module;
}

// Placeholder for compiling WAT to WASM (requires wasm-tools or similar)
inline std::vector<uint8_t> compile_wat(const char* wat_content, char* error_buf, uint32_t error_buf_size) {
    // In a real test environment, this would invoke wasm-tools or similar compiler
    // For now, this is a placeholder. Tests needing this will be skipped or use pre-compiled wasm.
    snprintf(error_buf, error_buf_size, "WAT compilation not implemented in test utils.");
    LOG_ERROR("WAT compilation not implemented in test utils. WAT: %s", wat_content);
    return {};
}


// Helper to find a resolved export by name
inline ResolvedComponentExportItem* find_export_by_name(WASMComponentInstanceInternal *comp_inst, const char *name) {
    if (!comp_inst || !name) return NULL;
    for (uint32 i = 0; i < comp_inst->num_resolved_exports; ++i) {
        if (comp_inst->resolved_exports[i].name && strcmp(comp_inst->resolved_exports[i].name, name) == 0) {
            return &comp_inst->resolved_exports[i];
        }
    }
    return NULL;
}

// TODO: Add more helper functions:
// - Functions to create various HostComponentValue types
// - Functions to compare HostComponentValue types
// - Wrapper to load component from WAT string or file, instantiate, etc.

#endif // _COMPONENT_TEST_UTILS_H
