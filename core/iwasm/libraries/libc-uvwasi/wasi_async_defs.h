#ifndef _WASI_ASYNC_DEFS_H
#define _WASI_ASYNC_DEFS_H

#include "uvwasi.h" // For uv_loop_t, uv_poll_t, uv_timer_t, uv_stream_t, uv_work_t etc.
#include "wasm_component_runtime.h" // For WASMComponentInstanceInternal, global_resource_table (if used directly)
#include "wasm_runtime.h"         // For WASMModuleInstance

// Forward declarations
struct wamr_wasi_future_s;
struct wamr_wasi_pollable_s;

// --- Enums for Types and States ---

typedef enum wamr_wasi_pollable_type_t {
    POLLABLE_TYPE_FD_READ,
    POLLABLE_TYPE_FD_WRITE,
    POLLABLE_TYPE_FUTURE,
    POLLABLE_TYPE_TIMEOUT,
    POLLABLE_TYPE_STREAM_READ, // Pollable associated with input stream readiness
    POLLABLE_TYPE_STREAM_WRITE // Pollable associated with output stream readiness
} wamr_wasi_pollable_type_t;

typedef enum wamr_wasi_future_state_t {
    FUTURE_STATE_PENDING,
    FUTURE_STATE_READY_OK,
    FUTURE_STATE_READY_ERR
} wamr_wasi_future_state_t;

typedef enum wamr_wasi_stream_type_t {
    STREAM_TYPE_FD,       // Standard file descriptor
    STREAM_TYPE_PIPE,     // Libuv pipe
    STREAM_TYPE_TCP,      // Libuv TCP stream
    STREAM_TYPE_CUSTOM    // Custom host-defined stream
} wamr_wasi_stream_type_t;

typedef enum wamr_wasi_stream_state_t {
    STREAM_STATE_OPEN,
    STREAM_STATE_CLOSED,
    STREAM_STATE_ERROR_READ,  // Read error occurred
    STREAM_STATE_ERROR_WRITE, // Write error occurred
    STREAM_STATE_ERROR_BOTH   // Errors on both ends or general error
} wamr_wasi_stream_state_t;

// Placeholder for WASI error codes (wasi_errno_t is usually from wasi_snapshot_preview1.h)
typedef uint16_t wasi_error_t; // Corresponds to wasi:io/error.code (enum ErrorCode)
#define WASI_ERRNO_SUCCESS  0  // Placeholder
#define WASI_ERRNO_AGAIN   6   // Placeholder for would_block / try_again
#define WASI_ERRNO_IO      29  // Placeholder for general I/O error
#define WASI_ERRNO_BADF    8   // Placeholder for bad file descriptor


// --- Host-Side C Structures ---

typedef struct wamr_wasi_pollable_s {
    wamr_wasi_pollable_type_t type;
    bool ready;                     // True if this specific pollable has fired
    void *user_data;                // Optional user data for callbacks or context
    uv_loop_t *loop;                // Libuv loop associated with this pollable
    WASMModuleInstance *module_inst; // Module instance that created/owns this (for callbacks, resource tracking)

    union {
        struct {
            uv_os_fd_t fd;          // System file descriptor
            uv_poll_t poll_watcher; // For FD_READ/WRITE using uv_poll
            uint32_t events;        // UV_READABLE | UV_WRITABLE
        } fd_poll;
        struct {
            struct wamr_wasi_future_s *future; // Pointer to the future it tracks
        } future_track;
        struct {
            uint64_t timeout_ns;    // Timeout in nanoseconds
            uv_timer_t timer_handle;
        } timeout;
        struct { // For stream pollables, might point to the stream itself or a specific watcher
            void *stream_ptr; // Points to wamr_wasi_input_stream_t or wamr_wasi_output_stream_t
            // uv_poll_t might be used if stream is FD-based and not a uv_stream_t
        } stream_poll;
    } data;

    // For linking multiple pollables in a single poll_oneoff call context
    struct wamr_wasi_pollable_s *next_in_poll_list;
    bool is_registered_in_poll_list; // Temp flag for poll_oneoff processing
    uint32_t wasm_resource_handle;   // Handle if this pollable is a registered resource
} wamr_wasi_pollable_t;


// Generic Future structure (conceptual)
// Specific C structs will be needed for each <ValueType, ErrorType> pair
// Example: wamr_wasi_future_bytes_error_t
typedef struct wamr_wasi_future_s {
    wamr_wasi_future_state_t state;
    wamr_wasi_pollable_t *result_pollable; // Pollable that becomes ready when this future resolves
    uv_loop_t *loop;                       // Libuv loop
    WASMModuleInstance *module_inst;       // Owning module instance context
    void* internal_ctx;                    // For storing context like uv_fs_t, uv_write_t, etc.
    uv_work_t work_req;                    // For futures resolved by background tasks
    uint32_t wasm_resource_handle;         // Handle if this future is a registered resource

    // The actual result union would be in specific future types, e.g.:
    // union { ValueType ok_value; ErrorType err_value; } result;
} wamr_wasi_future_t; // Base structure

// Example specific future: Future<list<u8>, error_code> (for stream read)
typedef struct wamr_wasi_future_bytes_error_s {
    wamr_wasi_future_t base; // Inherits common fields
    struct {
        uint8_t *bytes;
        uint64_t len;        // Actual number of bytes read/written
        bool end_of_stream;  // For stream reads
    } ok_value;
    wasi_error_t err_value;
} wamr_wasi_future_bytes_error_t;

// Example specific future: Future<u64, error_code> (for stream write, returns bytes written)
typedef struct wamr_wasi_future_u64_error_s {
    wamr_wasi_future_t base;
    uint64_t ok_value; // bytes written
    wasi_error_t err_value;
} wamr_wasi_future_u64_error_t;

// Example specific future: Future<void, error_code>
typedef struct wamr_wasi_future_void_error_s {
    wamr_wasi_future_t base;
    wasi_error_t err_value; // Only error is relevant for ok_value = void
} wamr_wasi_future_void_error_t;


// Input Stream
typedef struct wamr_wasi_input_stream_s {
    wamr_wasi_stream_type_t type;
    wamr_wasi_stream_state_t state;
    wasi_error_t last_error;
    WASMModuleInstance *module_inst; // Owning module instance
    uv_loop_t *loop;

    union {
        uv_stream_t *uv_s; // For libuv streams (uv_pipe_t, uv_tcp_t)
        uv_os_fd_t host_fd;  // For FD-backed streams not directly using uv_stream_t
    } handle;

    wamr_wasi_pollable_t *read_pollable;  // Pollable that becomes ready when readable
    uint32_t wasm_resource_handle;        // Handle if this stream is a registered resource
    // Internal buffering details are encapsulated or handled by libuv
} wamr_wasi_input_stream_t;

// Output Stream
typedef struct wamr_wasi_output_stream_s {
    wamr_wasi_stream_type_t type;
    wamr_wasi_stream_state_t state;
    wasi_error_t last_error;
    WASMModuleInstance *module_inst; // Owning module instance
    uv_loop_t *loop;

    union {
        uv_stream_t *uv_s;
        uv_os_fd_t host_fd;
    } handle;

    wamr_wasi_pollable_t *write_pollable; // Pollable that becomes ready when writable
    uint32_t wasm_resource_handle;         // Handle if this stream is a registered resource
} wamr_wasi_output_stream_t;


// Callback contexts for libuv async operations that resolve futures
typedef struct wamr_uv_future_ctx_s {
    wamr_wasi_future_t *future; // Points to the specific future to resolve
    // Add any other necessary data for the callback
    uint8_t *buffer_for_read;   // Buffer provided by user for stream read
    uint64_t buffer_len_for_read;
} wamr_uv_future_ctx_t;

// Destructor function declarations
void destroy_wamr_pollable(void *pollable_resource_data);
void destroy_wamr_future(void *future_resource_data); // Generic, might need type-specific if complex
void destroy_wamr_input_stream(void *stream_resource_data);
void destroy_wamr_output_stream(void *stream_resource_data);

#endif // _WASI_ASYNC_DEFS_H
