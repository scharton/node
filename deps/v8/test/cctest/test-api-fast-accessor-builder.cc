// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>

#include "include/v8.h"
#include "include/v8-experimental.h"

#include "src/api.h"
#include "test/cctest/cctest.h"

namespace {

// These tests mean to exercise v8::FastAccessorBuilder. Since initially the
// "native" accessor will get called, we need to "warmup" any accessor first,
// to make sure we're actually testing the v8::FastAccessorBuilder result.
// To accomplish this, we will
// - call each accesssor N times before the actual test.
// - wrap that call in a function, so that all such calls will go
//   through a single call site.
// - bloat that function with a very long comment to prevent its inlining.
// - register a native accessor which is different from the build one
//   (so that our tests will always fail if we don't end up in the 'fast'
//    accessor).
//
// FN_WARMUP(name, src) define a JS function "name" with body "src".
// It adds the INLINE_SPOILER to prevent inlining and will call name()
// repeatedly to guarantee it's "warm".
//
// Use:
//   CompileRun(FN_WARMUP("fn", "return something();"));
//   ExpectXXX("fn(1234)", 5678);

#define INLINE_SPOILER                                           \
  " /* "                                                         \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" \
  "*/ "  // 16 lines * 64 'X' =~ 1024 character comment.
#define FN_WARMUP(name, src)                  \
  "function " name "() { " src INLINE_SPOILER \
  " }; for(i = 0; i < 2; i++) { " name "() } "

static void NativePropertyAccessor(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_num(123));
}

}  // anonymous namespace


// Build a simple "fast accessor" and verify that it is being called.
TEST(FastAccessor) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::FunctionTemplate> foo = v8::FunctionTemplate::New(isolate);

  // Native accessor, bar, returns 123.
  foo->PrototypeTemplate()->SetAccessorProperty(
      v8_str("bar"),
      v8::FunctionTemplate::New(isolate, NativePropertyAccessor));

  // Fast accessor, barf, returns 124.
  auto fab = v8::experimental::FastAccessorBuilder::New(isolate);
  fab->ReturnValue(fab->IntegerConstant(124));
  foo->PrototypeTemplate()->SetAccessorProperty(
      v8_str("barf"), v8::FunctionTemplate::NewWithFastHandler(
                          isolate, NativePropertyAccessor, fab));

  // Install foo on the global object.
  CHECK(env->Global()
            ->Set(env.local(), v8_str("foo"),
                  foo->GetFunction(env.local()).ToLocalChecked())
            .FromJust());

  // Wrap f.barf + IC warmup.
  CompileRun(FN_WARMUP("barf", "f = new foo(); return f.barf"));

  ExpectInt32("f = new foo(); f.bar", 123);
  ExpectInt32("f = new foo(); f.barf", 123);  // First call in this call site.
  ExpectInt32("barf()", 124);                 // Call via warmed-up callsite.
}


void AddInternalFieldAccessor(v8::Isolate* isolate,
                              v8::Local<v8::Template> templ, const char* name,
                              int field_no) {
  auto builder = v8::experimental::FastAccessorBuilder::New(isolate);
  builder->ReturnValue(
      builder->LoadInternalField(builder->GetReceiver(), field_no));
  templ->SetAccessorProperty(v8_str(name),
                             v8::FunctionTemplate::NewWithFastHandler(
                                 isolate, NativePropertyAccessor, builder));
}


// "Fast" accessor that accesses an internal field.
TEST(FastAccessorWithInternalField) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::ObjectTemplate> foo = v8::ObjectTemplate::New(isolate);
  foo->SetInternalFieldCount(3);
  AddInternalFieldAccessor(isolate, foo, "field0", 0);
  AddInternalFieldAccessor(isolate, foo, "field1", 1);
  AddInternalFieldAccessor(isolate, foo, "field2", 2);

  // Create an instance w/ 3 internal fields, put in a string, a Smi, nothing.
  v8::Local<v8::Object> obj = foo->NewInstance(env.local()).ToLocalChecked();
  obj->SetInternalField(0, v8_str("Hi there!"));
  obj->SetInternalField(1, v8::Integer::New(isolate, 4321));
  CHECK(env->Global()->Set(env.local(), v8_str("obj"), obj).FromJust());

  // Warmup.
  CompileRun(FN_WARMUP("field0", "return obj.field0"));
  CompileRun(FN_WARMUP("field1", "return obj.field1"));
  CompileRun(FN_WARMUP("field2", "return obj.field2"));

  // Access fields.
  ExpectString("field0()", "Hi there!");
  ExpectInt32("field1()", 4321);
  ExpectUndefined("field2()");
}


// "Fast" accessor with control flow via ...OrReturnNull methods.
TEST(FastAccessorOrReturnNull) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::ObjectTemplate> foo = v8::ObjectTemplate::New(isolate);
  foo->SetInternalFieldCount(2);
  {
    // accessor "nullcheck": Return null if field 0 is non-null object; else 5.
    auto builder = v8::experimental::FastAccessorBuilder::New(isolate);
    auto val = builder->LoadInternalField(builder->GetReceiver(), 0);
    builder->CheckNotZeroOrReturnNull(val);
    builder->ReturnValue(builder->IntegerConstant(5));
    foo->SetAccessorProperty(v8_str("nullcheck"),
                             v8::FunctionTemplate::NewWithFastHandler(
                                 isolate, NativePropertyAccessor, builder));
  }
  {
    // accessor "maskcheck": Return null if field 1 has 3rd bit set.
    auto builder = v8::experimental::FastAccessorBuilder::New(isolate);
    auto val = builder->LoadInternalField(builder->GetReceiver(), 1);
    builder->CheckFlagSetOrReturnNull(val, 0x4);
    builder->ReturnValue(builder->IntegerConstant(42));
    foo->SetAccessorProperty(v8_str("maskcheck"),
                             v8::FunctionTemplate::NewWithFastHandler(
                                 isolate, NativePropertyAccessor, builder));
  }

  // Create an instance.
  v8::Local<v8::Object> obj = foo->NewInstance(env.local()).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("obj"), obj).FromJust());

  // CheckNotZeroOrReturnNull:
  CompileRun(FN_WARMUP("nullcheck", "return obj.nullcheck"));
  obj->SetAlignedPointerInInternalField(0, /* anything != nullptr */ isolate);
  ExpectInt32("nullcheck()", 5);
  obj->SetAlignedPointerInInternalField(0, nullptr);
  ExpectNull("nullcheck()");

  // CheckFlagSetOrReturnNull:
  CompileRun(FN_WARMUP("maskcheck", "return obj.maskcheck"));
  obj->SetAlignedPointerInInternalField(1, reinterpret_cast<void*>(0xf0));
  ExpectInt32("maskcheck()", 42);
  obj->SetAlignedPointerInInternalField(1, reinterpret_cast<void*>(0xfe));
  ExpectNull("maskcheck()");
}


// "Fast" accessor with simple control flow via explicit labels.
TEST(FastAccessorControlFlowWithLabels) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::ObjectTemplate> foo = v8::ObjectTemplate::New(isolate);
  foo->SetInternalFieldCount(1);
  {
    // accessor isnull: 0 for nullptr, else 1.
    auto builder = v8::experimental::FastAccessorBuilder::New(isolate);
    auto label = builder->MakeLabel();
    auto val = builder->LoadInternalField(builder->GetReceiver(), 0);
    builder->CheckNotZeroOrJump(val, label);
    builder->ReturnValue(builder->IntegerConstant(0));
    builder->SetLabel(label);
    builder->ReturnValue(builder->IntegerConstant(1));
    foo->SetAccessorProperty(v8_str("isnull"),
                             v8::FunctionTemplate::NewWithFastHandler(
                                 isolate, NativePropertyAccessor, builder));
  }

  // Create an instance.
  v8::Local<v8::Object> obj = foo->NewInstance(env.local()).ToLocalChecked();
  CHECK(env->Global()->Set(env.local(), v8_str("obj"), obj).FromJust());

  // CheckNotZeroOrReturnNull:
  CompileRun(FN_WARMUP("isnull", "return obj.isnull"));
  obj->SetAlignedPointerInInternalField(0, /* anything != nullptr */ isolate);
  ExpectInt32("isnull()", 1);
  obj->SetAlignedPointerInInternalField(0, nullptr);
  ExpectInt32("isnull()", 0);
}


// "Fast" accessor, loading things.
TEST(FastAccessorLoad) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::ObjectTemplate> foo = v8::ObjectTemplate::New(isolate);
  foo->SetInternalFieldCount(1);

  // Internal field 0 is a pointer to a C++ data structure that we wish to load
  // field values from.
  struct {
    size_t intval;
    v8::Local<v8::String> v8val;
  } val = {54321, v8_str("Hello")};

  {
    // accessor intisnonzero
    int intval_offset =
        static_cast<int>(reinterpret_cast<intptr_t>(&val.intval) -
                         reinterpret_cast<intptr_t>(&val));
    auto builder = v8::experimental::FastAccessorBuilder::New(isolate);
    auto label = builder->MakeLabel();
    auto val = builder->LoadValue(
        builder->LoadInternalField(builder->GetReceiver(), 0), intval_offset);
    builder->CheckNotZeroOrJump(val, label);
    builder->ReturnValue(builder->IntegerConstant(0));
    builder->SetLabel(label);
    builder->ReturnValue(builder->IntegerConstant(1));
    foo->SetAccessorProperty(v8_str("nonzero"),
                             v8::FunctionTemplate::NewWithFastHandler(
                                 isolate, NativePropertyAccessor, builder));
  }
  {
    // accessor loadval
    int v8val_offset = static_cast<int>(reinterpret_cast<intptr_t>(&val.v8val) -
                                        reinterpret_cast<intptr_t>(&val));
    auto builder = v8::experimental::FastAccessorBuilder::New(isolate);
    builder->ReturnValue(builder->LoadObject(
        builder->LoadInternalField(builder->GetReceiver(), 0), v8val_offset));
    foo->SetAccessorProperty(v8_str("loadval"),
                             v8::FunctionTemplate::NewWithFastHandler(
                                 isolate, NativePropertyAccessor, builder));
  }

  // Create an instance.
  v8::Local<v8::Object> obj = foo->NewInstance(env.local()).ToLocalChecked();
  obj->SetAlignedPointerInInternalField(0, &val);
  CHECK(env->Global()->Set(env.local(), v8_str("obj"), obj).FromJust());

  // Access val.intval:
  CompileRun(FN_WARMUP("nonzero", "return obj.nonzero"));
  ExpectInt32("nonzero()", 1);
  val.intval = 0;
  ExpectInt32("nonzero()", 0);
  val.intval = 27;
  ExpectInt32("nonzero()", 1);

  // Access val.v8val:
  CompileRun(FN_WARMUP("loadval", "return obj.loadval"));
  ExpectString("loadval()", "Hello");
}
