;; Conceptual WAT for a component.
;; Actual binary format or a different text format might be used if `wat2wasm` doesn't support this directly.
;; This file is primarily for documenting the test case structure.
;; We will assume a pre-compiled .wasm component file for the actual test if direct wat->component_wasm isn't trivial.

(component $simple_component
  ;; Core module that imports "foo" and defines "bar"
  (core module $module_A
    (import "env" "foo" (func $imported_foo (param i32) (result i32)))
    (func $bar (export "bar") (param i32) (result i32)
      local.get 0
      call $imported_foo
    )
  )

  ;; Instance of the core module
  (core instance $instance_A (instantiate $module_A))

  ;; Export "bar" from the component, sourced from instance_A's "bar"
  (export "bar" (func (core func $instance_A "bar")))

  ;; For testing purposes, we'd need to tell wamrc that the import "env" "foo"
  ;; should be treated as a cross-component call that requires a canonical ABI wrapper.
  ;; This might be done by:
  ;; 1. The component binary format itself indicating this (e.g., if "env" is an imported component interface).
  ;; 2. A manifest file.
  ;; 3. Test-specific modifications in the AOT compiler to force the flag.
  ;;
  ;; The wrapper `aot_component_wrapper_..._foo` would be generated for `$imported_foo`
  ;; when `$module_A` is compiled as part of this component.
)
