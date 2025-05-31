;; Conceptual WAT for a component testing primitive types.
(component $primitive_test_component
  (core module $module_C
    (import "env" "call_primitives" (func $imported_call_primitives
      (param i32) ;; s32
      (param i32) ;; u32
      (param i64) ;; s64
      (param i64) ;; u64
      (param f32)
      (param f64)
      (param i32) ;; bool (represented as i32 in core wasm)
      (param i32) ;; char (represented as i32 in core wasm)
      (result i32) ;; s32
      (result i32) ;; u32
      (result i64) ;; s64
      (result i64) ;; u64
      (result f32)
      (result f64)
      (result i32) ;; bool
      (result i32) ;; char
    ))

    (func $invoke_it (export "invoke_it")
      (param i32) (param i32) (param i64) (param i64)
      (param f32) (param f64) (param i32) (param i32)
      (result i32) (result i32) (result i64) (result i64)
      (result f32) (result f64) (result i32) (result i32)

      local.get 0
      local.get 1
      local.get 2
      local.get 3
      local.get 4
      local.get 5
      local.get 6
      local.get 7
      call $imported_call_primitives
      ;; Assume results are on stack and function signature matches for return
    )
  )

  (core instance $instance_C (instantiate $module_C))

  (export "invoke_it" (func (core func $instance_C "invoke_it")))

  ;; For testing, "env" "call_primitives" would be marked as cross-component.
  ;; The wrapper should use aot_canon_lower_value for params and aot_canon_lift_value for results.
)
