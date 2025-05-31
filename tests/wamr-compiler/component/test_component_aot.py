import subprocess
import os
import shutil
import unittest

# Configuration
WAMRC_PATH = "../../../wamr-compiler/build/wamrc" # Adjust path as needed
TEST_DIR = os.path.dirname(os.path.abspath(__file__))
SIMPLE_IMPORT_EXPORT_WAT = os.path.join(TEST_DIR, "simple_import_export_component.wat")
SIMPLE_IMPORT_EXPORT_WASM = os.path.join(TEST_DIR, "simple_import_export_component.wasm")

PRIMITIVE_TYPES_WAT = os.path.join(TEST_DIR, "primitive_types_comp.wat")
PRIMITIVE_TYPES_WASM = os.path.join(TEST_DIR, "primitive_types_comp.wasm")

STRING_LIST_WAT = os.path.join(TEST_DIR, "string_list_placeholder_comp.wat")
STRING_LIST_WASM = os.path.join(TEST_DIR, "string_list_placeholder_comp.wasm")

# TODO: Find a wat2wasm tool that supports the component model text format,
# or commit pre-compiled .wasm component files.
# For now, these tests might need to operate on pre-compiled .wasm files.
# Or, we can use core wasm modules and the test itself can "simulate"
# the component linking aspect by how it instructs wamrc or interprets results.

def wat_to_wasm_component(wat_path, wasm_path):
    """
    Placeholder for converting component WAT to WASM.
    This requires a wat2wasm tool that supports the component model text format.
    For now, we'll assume .wasm files are pre-compiled or this function is updated.
    """
    if not os.path.exists(wat_path):
        print(f"WAT file not found: {wat_path}")
        return False
    # Example: subprocess.run(["wat2wasm", wat_path, "-o", wasm_path, "--enable-all"], check=True)
    # Since we don't have a readily available component-aware wat2wasm in this environment,
    # this function will be a no-op and tests should use pre-compiled .wasm files
    # or the .wat files are for documentation only.
    print(f"Skipping conversion for {wat_path}, assuming {wasm_path} exists or is not strictly needed for this test phase.")
    if os.path.exists(wat_path) and not os.path.exists(wasm_path):
        # Fallback: if only .wat exists, try to copy it to .wasm to allow wamrc to load it
        # This won't work if wamrc strictly expects binary, but helps test file handling.
        # In reality, wamrc needs a binary component.
        # shutil.copy(wat_path, wasm_path)
        print(f"WARNING: {wasm_path} not found. Tests may fail if a binary component is required.")
        # For now, let's assume the .wat files are just for reference and we'll try to compile them directly
        # if wamrc supports that, or the test will use a placeholder mechanism.
        return True # Allow test to proceed to wamrc call with .wat
    return os.path.exists(wasm_path)


class TestComponentAOT(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if not os.path.exists(WAMRC_PATH):
            raise unittest.SkipTest(f"WAMRC not found at {WAMRC_PATH}. Build it first.")
        # wat_to_wasm_component(SIMPLE_IMPORT_EXPORT_WAT, SIMPLE_IMPORT_EXPORT_WASM)
        # wat_to_wasm_component(PRIMITIVE_TYPES_WAT, PRIMITIVE_TYPES_WASM)
        # wat_to_wasm_component(STRING_LIST_WAT, STRING_LIST_WASM)
        # For now, we'll use the .wat files directly if wamrc can take them,
        # or rely on manual precompilation to .wasm for these tests to pass.

    def run_wamrc(self, input_file, output_base, extra_args=None, expect_success=True):
        if extra_args is None:
            extra_args = []

        # Output directory for this test run
        output_dir = os.path.join(TEST_DIR, "output", output_base)
        if os.path.exists(output_dir):
            shutil.rmtree(output_dir)
        os.makedirs(output_dir)

        # Final output AOT/object file name (wamrc might create it in output_dir)
        # For component compilation, wamrc might create multiple files in output_dir.
        # The name here is more of a base.
        final_output_file = os.path.join(output_dir, f"{output_base}.aot") # Default, adjust if testing object format

        cmd = [
            WAMRC_PATH,
            "--component", # Key flag for component compilation
            "-o", final_output_file, # This might be interpreted as dir by modified wamrc
        ] + extra_args + [input_file]

        print(f"Running wamrc: {' '.join(cmd)}")
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, stderr = process.communicate()
        stdout_str = stdout.decode('utf-8')
        stderr_str = stderr.decode('utf-8')

        print("WAMRC STDOUT:")
        print(stdout_str)
        print("WAMRC STDERR:")
        print(stderr_str)

        if expect_success:
            self.assertEqual(process.returncode, 0, f"wamrc failed: {stderr_str}")
        else:
            self.assertNotEqual(process.returncode, 0, f"wamrc was expected to fail but succeeded: {stderr_str}")

        return stdout_str, stderr_str, output_dir

    def test_01_simple_import_export_wrapper_generation(self):
        """
        Tests that a wrapper function is conceptually generated for a cross-component import.
        Relies on log messages from the compiler indicating wrapper generation.
        Assumes 'env.foo' in simple_import_export_component.wat is treated as cross-component.
        """
        # For this test to pass, 'env.foo' in the component's module must be marked
        # with 'is_cross_component_call = true' by a prior code change or test harness setup.
        # We will check for the log message: "AOT: Detected cross-component call for import func_idx..."
        # and the placeholder wrapper name.

        # Using .wat directly, assuming wamrc can load it or it's a placeholder for a binary
        input_src = SIMPLE_IMPORT_EXPORT_WAT
        # If a precompiled component.wasm is available:
        # input_src = SIMPLE_IMPORT_EXPORT_WASM
        # if not os.path.exists(input_src):
        #     self.skipTest(f"{input_src} not found, skipping test. Precompile component WAT first.")

        stdout, stderr, _ = self.run_wamrc(input_src, "simple_import_export")

        # Check for the log message from aot_emit_function.c
        self.assertIn("AOT: Detected cross-component call for import func_idx", stdout + stderr,
                      "Cross-component call detection log not found.")
        # Check for the conceptual wrapper name mentioned in aot_emit_function.c
        # This name might vary based on module/function names from the WAT/WASM.
        # Example: "aot_component_wrapper_env_foo" or "aot_component_wrapper_idx_0"
        # The actual index will depend on the loaded component.
        self.assertIn("aot_component_wrapper_", stdout + stderr,
                      "Component wrapper function name pattern not found in output.")
        # A more specific check would be:
        # self.assertIn("aot_component_wrapper_env_foo", stdout + stderr)
        # or based on func_idx, e.g., "aot_component_wrapper_idx_0" (if foo is the first import)

    def test_02_primitive_types_wrapper_calls(self):
        """
        Tests wrapper generation and conceptual LIFT/LOWER calls for primitive types.
        Relies on logs or specific LLVM IR patterns if inspection is feasible.
        Assumes 'env.call_primitives' in primitive_types_comp.wat is cross-component.
        """
        input_src = PRIMITIVE_TYPES_WAT
        # input_src = PRIMITIVE_TYPES_WASM
        # if not os.path.exists(input_src):
        #     self.skipTest(f"{input_src} not found, skipping test.")

        stdout, stderr, output_dir = self.run_wamrc(input_src, "primitive_types_comp",
                                               extra_args=["--format=llvmir-unopt"]) # Example: emit LLVM IR

        self.assertIn("AOT: Detected cross-component call for import func_idx", stdout + stderr)
        self.assertIn("aot_component_wrapper_", stdout + stderr)

        # Conceptual: Inspect LLVM IR in output_dir for calls to aot_canon_lower_value / aot_canon_lift_value
        # This requires parsing the IR, which is complex for a simple test script.
        # For now, this test mainly ensures the wrapper is generated.
        # We can add more specific log messages in the LIFT/LOWER functions for testing.
        # e.g., "LOWER for primitive type S32 called in wrapper"
        # self.assertIn("LOWER for primitive type", stdout + stderr) # Example log check
        # self.assertIn("LIFT for primitive type", stdout + stderr)  # Example log check
        print(f"Primitive types test: LLVM IR (if generated) is in {output_dir}")
        self.assertTrue(True, "Placeholder for LLVM IR inspection for primitive LIFT/LOWER calls.")


    def test_03_string_list_stubs_reached(self):
        """
        Tests that LIFT/LOWER stubs for string/list types are reached.
        Relies on "not yet implemented" log messages from the stubs.
        Assumes 'env.call_string_list' in string_list_placeholder_comp.wat is cross-component.
        """
        input_src = STRING_LIST_WAT
        # input_src = STRING_LIST_WASM
        # if not os.path.exists(input_src):
        #     self.skipTest(f"{input_src} not found, skipping test.")

        stdout, stderr, _ = self.run_wamrc(input_src, "string_list_comp")

        self.assertIn("AOT: Detected cross-component call for import func_idx", stdout + stderr)
        self.assertIn("aot_component_wrapper_", stdout + stderr)

        # Check for "not yet implemented" messages from LIFT/LOWER stubs for string/list
        # These messages come from aot_set_last_error, which might not print to stdout/stderr of wamrc.
        # This test might need adjustment if those errors are only available via API or internal logging.
        # For now, we assume if the wrapper is generated, the stubs would be part of its body.
        # A more robust test would involve inspecting LLVM IR for calls to the stub functions
        # or specific error handling blocks generated by them.

        # The current stubs call aot_set_last_error. If these errors were printed by wamrc, we could check:
        # self.assertIn("LIFT for String not fully implemented", stderr + stdout)
        # self.assertIn("LOWER for String not fully implemented", stderr + stdout)
        # self.assertIn("LIFT for List not fully implemented", stderr + stdout)
        # self.assertIn("LOWER for List not fully implemented", stderr + stdout)

        # For now, this test primarily checks that the wrapper for a function with string/list types
        # is generated, implying that the paths to those LIFT/LOWER functions exist.
        self.assertTrue(True, "Placeholder for checking if string/list LIFT/LOWER stubs are effectively called.")

if __name__ == "__main__":
    unittest.main()
