// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/cctest/wasm/wasm-module-runner.h"

#include "src/handles.h"
#include "src/isolate.h"
#include "src/objects.h"
#include "src/property-descriptor.h"
#include "src/wasm/module-decoder.h"
#include "src/wasm/wasm-interpreter.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-result.h"
#include "src/zone.h"

namespace v8 {
namespace internal {
namespace wasm {
namespace testing {

uint32_t GetMinModuleMemSize(const WasmModule* module) {
  return WasmModule::kPageSize * module->min_mem_pages;
}

const WasmModule* DecodeWasmModuleForTesting(Isolate* isolate, Zone* zone,
                                             ErrorThrower& thrower,
                                             const byte* module_start,
                                             const byte* module_end,
                                             ModuleOrigin origin) {
  // Decode the module, but don't verify function bodies, since we'll
  // be compiling them anyway.
  ModuleResult decoding_result =
      DecodeWasmModule(isolate, zone, module_start, module_end, false, origin);

  std::unique_ptr<const WasmModule> module(decoding_result.val);
  if (decoding_result.failed()) {
    // Module verification failed. throw.
    thrower.Error("WASM.compileRun() failed: %s",
                  decoding_result.error_msg.get());
    return nullptr;
  }

  if (thrower.error()) return nullptr;
  return module.release();
}

const Handle<JSObject> InstantiateModuleForTesting(Isolate* isolate,
                                                   ErrorThrower& thrower,
                                                   const WasmModule* module) {
  CHECK(module != nullptr);

  if (module->import_table.size() > 0) {
    thrower.Error("Not supported: module has imports.");
  }
  if (module->export_table.size() == 0) {
    thrower.Error("Not supported: module has no exports.");
  }

  if (thrower.error()) return Handle<JSObject>::null();

  MaybeHandle<FixedArray> compiled_module =
      module->CompileFunctions(isolate, &thrower);

  if (compiled_module.is_null()) return Handle<JSObject>::null();
  return WasmModule::Instantiate(isolate, compiled_module.ToHandleChecked(),
                                 Handle<JSReceiver>::null(),
                                 Handle<JSArrayBuffer>::null())
      .ToHandleChecked();
}

int32_t CompileAndRunWasmModule(Isolate* isolate, const byte* module_start,
                                const byte* module_end, bool asm_js) {
  HandleScope scope(isolate);
  Zone zone(isolate->allocator());

  ErrorThrower thrower(isolate, "CompileAndRunWasmModule");
  std::unique_ptr<const WasmModule> module(DecodeWasmModuleForTesting(
      isolate, &zone, thrower, module_start, module_end,
      asm_js ? kAsmJsOrigin : kWasmOrigin));

  if (module == nullptr) {
    return -1;
  }
  Handle<JSObject> instance =
      InstantiateModuleForTesting(isolate, thrower, module.get());
  if (instance.is_null()) {
    return -1;
  }
  return CallWasmFunctionForTesting(isolate, instance, thrower,
                                    asm_js ? "caller" : "main", 0, nullptr,
                                    asm_js);
}

int32_t InterpretWasmModule(Isolate* isolate, ErrorThrower& thrower,
                            const WasmModule* module, int function_index,
                            WasmVal* args) {
  CHECK(module != nullptr);

  Zone zone(isolate->allocator());
  v8::internal::HandleScope scope(isolate);

  if (module->import_table.size() > 0) {
    thrower.Error("Not supported: module has imports.");
  }
  if (module->export_table.size() == 0) {
    thrower.Error("Not supported: module has no exports.");
  }

  if (thrower.error()) return -1;

  WasmModuleInstance instance(module);
  instance.context = isolate->native_context();
  instance.mem_size = GetMinModuleMemSize(module);
  instance.mem_start = nullptr;
  instance.globals_start = nullptr;

  ModuleEnv module_env;
  module_env.module = module;
  module_env.instance = &instance;
  module_env.origin = module->origin;

  const WasmFunction* function = &(module->functions[function_index]);

  FunctionBody body = {&module_env, function->sig, module->module_start,
                       module->module_start + function->code_start_offset,
                       module->module_start + function->code_end_offset};
  DecodeResult result = VerifyWasmCode(isolate->allocator(), body);
  if (result.failed()) {
    thrower.Error("Function did not verify");
    return -1;
  }

  WasmInterpreter interpreter(&instance, isolate->allocator());

  WasmInterpreter::Thread* thread = interpreter.GetThread(0);
  thread->Reset();
  thread->PushFrame(function, args);
  if (thread->Run() == WasmInterpreter::FINISHED) {
    WasmVal val = thread->GetReturnValue();
    return val.to<int32_t>();
  } else if (thread->state() == WasmInterpreter::TRAPPED) {
    return 0xdeadbeef;
  } else {
    thrower.Error("Interpreter did not finish execution within its step bound");
    return -1;
  }
}

int32_t CallWasmFunctionForTesting(Isolate* isolate, Handle<JSObject> instance,
                                   ErrorThrower& thrower, const char* name,
                                   int argc, Handle<Object> argv[],
                                   bool asm_js) {
  Handle<JSObject> exports_object;
  if (asm_js) {
    exports_object = instance;
  } else {
    Handle<Name> exports = isolate->factory()->InternalizeUtf8String("exports");
    exports_object = Handle<JSObject>::cast(
        JSObject::GetProperty(instance, exports).ToHandleChecked());
  }
  Handle<Name> main_name = isolate->factory()->NewStringFromAsciiChecked(name);
  PropertyDescriptor desc;
  Maybe<bool> property_found = JSReceiver::GetOwnPropertyDescriptor(
      isolate, exports_object, main_name, &desc);
  if (!property_found.FromMaybe(false)) return -1;

  Handle<JSFunction> main_export = Handle<JSFunction>::cast(desc.value());

  // Call the JS function.
  Handle<Object> undefined = isolate->factory()->undefined_value();
  MaybeHandle<Object> retval =
      Execution::Call(isolate, main_export, undefined, argc, argv);

  // The result should be a number.
  if (retval.is_null()) {
    thrower.Error("WASM.compileRun() failed: Invocation was null");
    return -1;
  }
  Handle<Object> result = retval.ToHandleChecked();
  if (result->IsSmi()) {
    return Smi::cast(*result)->value();
  }
  if (result->IsHeapNumber()) {
    return static_cast<int32_t>(HeapNumber::cast(*result)->value());
  }
  thrower.Error("WASM.compileRun() failed: Return value should be number");
  return -1;
}

}  // namespace testing
}  // namespace wasm
}  // namespace internal
}  // namespace v8
