/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstring>

#include <executorch/runtime/backend/backend_execution_context.h>
#include <executorch/runtime/backend/backend_init_context.h>
#include <executorch/runtime/backend/backend_option_context.h>
#include <executorch/runtime/backend/options.h>
#include <executorch/runtime/core/array_ref.h>
#include <executorch/runtime/core/error.h>
#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/core/freeable_buffer.h>
#include <executorch/runtime/core/memory_allocator.h>
#include <executorch/runtime/core/named_data_map.h>
#include <executorch/runtime/core/result.h>
#include <executorch/runtime/platform/compiler.h>

namespace executorch {
namespace ET_RUNTIME_NAMESPACE {

struct SizedBuffer {
  void* buffer;
  size_t nbytes; // number of bytes of buffer
};

struct CompileSpec {
  const char* key; // spec key
  SizedBuffer value; // spec value
};

/**
 * An opaque handle managed by a backend. Typically points to a backend-private
 * class/struct.
 */
using DelegateHandle = void;

class BackendInterface {
 public:
  virtual ~BackendInterface() = 0;

  /**
   * Returns true if the backend is available to process delegation calls.
   */
  ET_NODISCARD virtual bool is_available() const = 0;

  /**
   * Responsible to further process (compile/transform/optimize) the compiled
   * unit that was produced, ahead-of-time, as well as perform any backend
   * initialization to ready it for execution. This method is called every time
   * the ExecuTorch program is initialized. Consequently, this is the place to
   * perform any backend initialization as well as transformations,
   * optimizations, and even compilation that depend on the target device. As
   * such, it is strongly encouraged to push as much processing as possible to
   * the ahead-of-time processing.
   *
   * @param[in] processed An opaque (to ExecuTorch) backend-specific compiled
   *     unit from the preprocessor. Can contain anything the backend needs to
   *     execute the equivalent semantics of the passed-in Module and its
   *     method. Often passed unmodified to `execute()` as a `DelegateHandle`,
   *     unless it needs further processing at init time to be fully executable.
   *     If the data is not needed after init(), calling processed->Free() can
   *     reclaim its memory.
   * @param[in] compile_specs The exact same compiler specification that
   *     was used ahead-of-time to produce `processed`.
   *
   * @returns On success, an opaque handle representing the the method
   *     implemented by the delegate. This handle is passed to `execute()` and
   *     `destroy()`, and the memory it points to is owned by the backend.
   *     Typically points to a backend-private class/struct.
   * @returns On error, returns an error code other than Error::Ok. If the
   *     compiled unit (the preprocessed result from ahead of time) is not
   *     compatible with the current backend runtime, return the error code
   *     Error::DelegateInvalidCompatibility. Other backend delegate
   *     specific error codes can be found in error.h.
   */
  ET_NODISCARD virtual Result<DelegateHandle*> init(
      BackendInitContext& context,
      FreeableBuffer* processed,
      ArrayRef<CompileSpec> compile_specs) const = 0;

  /**
   * Responsible for executing the given method’s handle, as it was produced
   * by compile.
   *
   * @param[in] handle An opaque handle returned by `init()`. Usually a backend
   *     executable unit. This executable unit should be ready to execute the
   *     delegate blobs.
   * @param[in] args The method’s inputs and outputs.
   * @retval Error::Ok if successful.
   */
  ET_NODISCARD virtual Error execute(
      BackendExecutionContext& context,
      DelegateHandle* handle,
      EValue** args) const = 0;

  /**
   * Responsible update the backend status, if any. The backend options are
   * passed in by users, and the backend can update its internal status based on
   * the options.
   *
   * @param[in] context Runtime context if any. Currently it's not used.
   * @param[in] args A list of BackendOptions passed in by users.
   * @retval Error::Ok if successful.
   */
  ET_NODISCARD virtual Error set_option(
      __ET_UNUSED BackendOptionContext& context,
      const executorch::runtime::Span<BackendOption>& backend_options) {
    return Error::Ok;
  };

  /**
   * Responsible update the backend status, if any. The backend options are
   * passed in by users, and the backend can update its internal status based on
   * the options.
   *
   * @param[in] context Runtime context if any. Currently it's not used.
   * @param[in] args A list of BackendOptions passed in by users, that will be
   * filled by the backend
   * @retval Error::Ok if successful.
   */
  ET_NODISCARD virtual Error get_option(
      __ET_UNUSED BackendOptionContext& context,
      executorch::runtime::Span<BackendOption>& backend_options) {
    return Error::Ok;
  };

  /**
   * Responsible for destroying a handle, if it's required for some backend.
   * It may be needed for some backends. For example, resources associated with
   * this handle needs to be released. This method is called when the execution
   * plan is destroyed (i.e., the program is out of its lifespan).
   *
   * @param[in] handle The handle to be destroyed. An opaque handle returned by
   *     `init()`.
   */
  virtual void destroy(ET_UNUSED DelegateHandle* handle) const {}
};

/**
 * Returns the corresponding object pointer for a given string name.
 * The mapping is populated using register_backend method.
 *
 * @param[in] name Name of the user-defined backend delegate.
 * @retval Pointer to the appropriate object that implements BackendInterface.
 *         Nullptr if it can't find anything with the given name.
 */
BackendInterface* get_backend_class(const char* name);

/**
 * A named instance of a backend.
 */
struct Backend {
  /// The name of the backend. Must match the string used in the PTE file.
  const char* name;
  /// The instance of the backend to use when loading and executing programs.
  BackendInterface* backend;
};

/**
 * Registers the Backend object (i.e. string name and BackendInterface pair) so
 * that it could be called via the name during the runtime.
 *
 * @param[in] backend Backend object
 * @retval Error code representing whether registration was successful.
 */
ET_NODISCARD Error register_backend(const Backend& backend);

/**
 * Returns the number of registered backends.
 */
size_t get_num_registered_backends();

/**
 * Returns the backend name at the given index.
 */
Result<const char*> get_backend_name(size_t index);

/**
 * Sets backend options for a specific backend.
 *
 * @param backend_name The name of the backend to set options for
 * @param backend_options The backend option list containing the options
 * to set
 * @return Error::Ok on success, Error::NotFound if backend is not found, or
 * other error codes on failure
 */
Error set_option(
    const char* backend_name,
    const executorch::runtime::Span<executorch::runtime::BackendOption>
        backend_options);

/**
 * Retrieves backend options for a specific backend.
 *
 * @param backend_name The name of the backend to get options from
 * @param backend_options The backend option objects that will be filled with
 * the populated values from the backend
 * @return Error::Ok on success, Error::NotFound if backend is not found, or
 * other error codes on failure
 */
Error get_option(
    const char* backend_name,
    executorch::runtime::Span<executorch::runtime::BackendOption>
        backend_options);

} // namespace ET_RUNTIME_NAMESPACE
} // namespace executorch

namespace torch {
namespace executor {
// TODO(T197294990): Remove these deprecated aliases once all users have moved
// to the new `::executorch` namespaces.
using ::executorch::ET_RUNTIME_NAMESPACE::Backend;
using ::executorch::ET_RUNTIME_NAMESPACE::CompileSpec;
using ::executorch::ET_RUNTIME_NAMESPACE::DelegateHandle;
using ::executorch::ET_RUNTIME_NAMESPACE::get_backend_class;
using ::executorch::ET_RUNTIME_NAMESPACE::register_backend;
using ::executorch::ET_RUNTIME_NAMESPACE::SizedBuffer;
using PyTorchBackendInterface =
    ::executorch::ET_RUNTIME_NAMESPACE::BackendInterface;
} // namespace executor
} // namespace torch
