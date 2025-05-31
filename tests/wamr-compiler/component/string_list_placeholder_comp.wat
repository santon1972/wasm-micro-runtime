;; Conceptual WAT for a component testing string/list placeholder stubs.
(component $string_list_test_component
  (core module $module_E
    ;; Placeholder for string: represented as (offset, length) pair of i32s in core Wasm
    ;; Placeholder for list<i32>: also (offset, length) pair of i32s
    (import "env" "call_string_list" (func $imported_call_string_list
      (param i32 i32) ;; string (offset, length)
      (param i32 i32) ;; list<i32> (offset, length)
      (result i32 i32) ;; string (offset, length)
    ))

    (func $invoke_it (export "invoke_it")
      (param i32 i32) ;; string_offset, string_length
      (param i32 i32) ;; list_offset, list_length
      (result i32 i32) ;; string_offset, string_length

      local.get 0 ;; string_offset
      local.get 1 ;; string_length
      local.get 2 ;; list_offset
      local.get 3 ;; list_length
      call $imported_call_string_list
    )
  )

  (core instance $instance_E (instantiate $module_E))

  (export "invoke_it" (func (core func $instance_E "invoke_it")))

  ;; For testing, "env" "call_string_list" would be marked as cross-component.
  ;; The wrapper should hit the stubs for lift_string, lift_list, lower_string.
)
