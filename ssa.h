#pragma once

#include "api.h"

/// Append a variable to the instruction trace (no operand)
extern uint32_t jit_trace_append(uint32_t type,
                          const char *cmd);

/// Append a variable to the instruction trace (1 operand)
extern uint32_t jit_trace_append(uint32_t type,
                          const char *cmd,
                          uint32_t arg1);

/// Append a variable to the instruction trace (2 operands)
extern uint32_t jit_trace_append(uint32_t type,
                          const char *cmd,
                          uint32_t arg1,
                          uint32_t arg2);

/// Append a variable to the instruction trace (3 operands)
extern uint32_t jit_trace_append(uint32_t type,
                          const char *cmd,
                          uint32_t arg1,
                          uint32_t arg2,
                          uint32_t arg3);

/// Register an existing variable with the JIT compiler
extern uint32_t jit_var_register(uint32_t type,
                          void *ptr,
                          size_t size,
                          bool free);

/// Register pointer literal as a special variable within the JIT compiler
extern uint32_t jit_var_register_ptr(const void *ptr);

/// Copy a memory region onto the device and return its variable index
extern uint32_t jit_var_copy_to_device(uint32_t type,
                                const void *value,
                                size_t size);

/// Increase the internal reference count of a given variable
extern void jit_inc_ref_int(uint32_t index);

/// Decrease the internal reference count of a given variable
extern void jit_dec_ref_int(uint32_t index);

/// Increase the external reference count of a given variable
extern void jit_inc_ref_ext(uint32_t index);

/// Decrease the external reference count of a given variable
extern void jit_dec_ref_ext(uint32_t index);

// Query the pointer variable associated with a given variable
extern void *jit_var_ptr(uint32_t index);

// Query the size of a given variable
extern size_t jit_var_size(uint32_t index);

/// Set the size of a given variable (if possible, otherwise throw)
extern uint32_t jit_var_set_size(uint32_t index, size_t size, bool copy);

/// Assign a descriptive label to a given variable
extern void jit_var_set_label(uint32_t index, const char *label);

/// Query the descriptive label associated with a given variable
extern const char *jit_var_label(uint32_t index);

/// Return the size of a given variable type
extern size_t jit_type_size(uint32_t type);

/// Migrate a variable to a different flavor of memory
extern void jit_var_migrate(uint32_t idx, AllocType type);

/// Indicate that evaluation of the given variable causes side effects
extern void jit_var_mark_side_effect(uint32_t index);

/// Mark variable as dirty, e.g. because of pending scatter operations
extern void jit_var_mark_dirty(uint32_t index);

/// Return a human-readable summary of registered variables
extern const char *jit_whos();
