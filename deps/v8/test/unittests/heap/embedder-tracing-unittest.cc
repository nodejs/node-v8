// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/embedder-tracing.h"
#include "test/unittests/test-utils.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace v8 {
namespace internal {

using LocalEmbedderHeapTracerWithIsolate = TestWithIsolate;

namespace heap {

using testing::StrictMock;
using testing::_;
using testing::Return;
using v8::EmbedderHeapTracer;
using v8::internal::LocalEmbedderHeapTracer;

namespace {

LocalEmbedderHeapTracer::WrapperInfo CreateWrapperInfo() {
  return LocalEmbedderHeapTracer::WrapperInfo(nullptr, nullptr);
}

}  // namespace

class MockEmbedderHeapTracer : public EmbedderHeapTracer {
 public:
  MOCK_METHOD0(TracePrologue, void());
  MOCK_METHOD0(TraceEpilogue, void());
  MOCK_METHOD0(AbortTracing, void());
  MOCK_METHOD0(EnterFinalPause, void());
  MOCK_METHOD0(IsTracingDone, bool());
  MOCK_METHOD1(RegisterV8References,
               void(const std::vector<std::pair<void*, void*> >&));
  MOCK_METHOD2(AdvanceTracing,
               bool(double deadline_in_ms, AdvanceTracingActions actions));
};

TEST(LocalEmbedderHeapTracer, InUse) {
  MockEmbedderHeapTracer mock_remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&mock_remote_tracer);
  EXPECT_TRUE(local_tracer.InUse());
}

TEST(LocalEmbedderHeapTracer, NoRemoteTracer) {
  LocalEmbedderHeapTracer local_tracer(nullptr);
  // We should be able to call all functions without a remote tracer being
  // attached.
  EXPECT_FALSE(local_tracer.InUse());
  local_tracer.TracePrologue();
  local_tracer.EnterFinalPause();
  bool more_work = local_tracer.Trace(
      0, EmbedderHeapTracer::AdvanceTracingActions(
             EmbedderHeapTracer::ForceCompletionAction::FORCE_COMPLETION));
  EXPECT_FALSE(more_work);
  local_tracer.TraceEpilogue();
}

TEST(LocalEmbedderHeapTracer, TracePrologueForwards) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  EXPECT_CALL(remote_tracer, TracePrologue());
  local_tracer.TracePrologue();
}

TEST(LocalEmbedderHeapTracer, TraceEpilogueForwards) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  EXPECT_CALL(remote_tracer, TraceEpilogue());
  local_tracer.TraceEpilogue();
}

TEST(LocalEmbedderHeapTracer, AbortTracingForwards) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  EXPECT_CALL(remote_tracer, AbortTracing());
  local_tracer.AbortTracing();
}

TEST(LocalEmbedderHeapTracer, AbortTracingClearsCachedWrappers) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  local_tracer.AddWrapperToTrace(CreateWrapperInfo());
  EXPECT_CALL(remote_tracer, AbortTracing());
  local_tracer.AbortTracing();
  EXPECT_EQ(0u, local_tracer.NumberOfCachedWrappersToTrace());
}

TEST(LocalEmbedderHeapTracer, EnterFinalPauseForwards) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  EXPECT_CALL(remote_tracer, EnterFinalPause());
  local_tracer.EnterFinalPause();
}

TEST(LocalEmbedderHeapTracer, IsRemoteTracingDoneIncludesRemote) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  EXPECT_CALL(remote_tracer, IsTracingDone());
  local_tracer.IsRemoteTracingDone();
}

TEST(LocalEmbedderHeapTracer, NumberOfCachedWrappersToTraceExcludesRemote) {
  LocalEmbedderHeapTracer local_tracer(nullptr);
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  local_tracer.SetRemoteTracer(&remote_tracer);
  local_tracer.NumberOfCachedWrappersToTrace();
}

TEST(LocalEmbedderHeapTracer, RegisterWrappersWithRemoteTracer) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  local_tracer.AddWrapperToTrace(CreateWrapperInfo());
  EXPECT_EQ(1u, local_tracer.NumberOfCachedWrappersToTrace());
  EXPECT_CALL(remote_tracer, RegisterV8References(_));
  local_tracer.RegisterWrappersWithRemoteTracer();
  EXPECT_EQ(0u, local_tracer.NumberOfCachedWrappersToTrace());
  EXPECT_CALL(remote_tracer, IsTracingDone()).WillOnce(Return(false));
  EXPECT_FALSE(local_tracer.IsRemoteTracingDone());
}

TEST(LocalEmbedderHeapTracer, TraceFinishes) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  local_tracer.AddWrapperToTrace(CreateWrapperInfo());
  EXPECT_EQ(1u, local_tracer.NumberOfCachedWrappersToTrace());
  EXPECT_CALL(remote_tracer, RegisterV8References(_));
  local_tracer.RegisterWrappersWithRemoteTracer();
  EXPECT_CALL(remote_tracer, AdvanceTracing(0, _)).WillOnce(Return(false));
  EXPECT_FALSE(local_tracer.Trace(
      0, EmbedderHeapTracer::AdvanceTracingActions(
             EmbedderHeapTracer::ForceCompletionAction::FORCE_COMPLETION)));
  EXPECT_EQ(0u, local_tracer.NumberOfCachedWrappersToTrace());
}

TEST(LocalEmbedderHeapTracer, TraceDoesNotFinish) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(nullptr);
  local_tracer.SetRemoteTracer(&remote_tracer);
  local_tracer.AddWrapperToTrace(CreateWrapperInfo());
  EXPECT_EQ(1u, local_tracer.NumberOfCachedWrappersToTrace());
  EXPECT_CALL(remote_tracer, RegisterV8References(_));
  local_tracer.RegisterWrappersWithRemoteTracer();
  EXPECT_CALL(remote_tracer, AdvanceTracing(0, _)).WillOnce(Return(true));
  EXPECT_TRUE(local_tracer.Trace(
      0, EmbedderHeapTracer::AdvanceTracingActions(
             EmbedderHeapTracer::ForceCompletionAction::FORCE_COMPLETION)));
  EXPECT_EQ(0u, local_tracer.NumberOfCachedWrappersToTrace());
}

TEST_F(LocalEmbedderHeapTracerWithIsolate, SetRemoteTracerSetsIsolate) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  LocalEmbedderHeapTracer local_tracer(isolate());
  local_tracer.SetRemoteTracer(&remote_tracer);
  EXPECT_EQ(isolate(), reinterpret_cast<Isolate*>(remote_tracer.isolate()));
}

TEST_F(LocalEmbedderHeapTracerWithIsolate, DestructorClearsIsolate) {
  StrictMock<MockEmbedderHeapTracer> remote_tracer;
  {
    LocalEmbedderHeapTracer local_tracer(isolate());
    local_tracer.SetRemoteTracer(&remote_tracer);
    EXPECT_EQ(isolate(), reinterpret_cast<Isolate*>(remote_tracer.isolate()));
  }
  EXPECT_EQ(nullptr, remote_tracer.isolate());
}

}  // namespace heap
}  // namespace internal
}  // namespace v8
