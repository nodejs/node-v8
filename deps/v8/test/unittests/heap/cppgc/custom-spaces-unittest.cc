// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/cppgc/allocation.h"
#include "include/cppgc/custom-space.h"
#include "src/heap/cppgc/heap-page.h"
#include "src/heap/cppgc/raw-heap.h"
#include "test/unittests/heap/cppgc/tests.h"

namespace cppgc {

class CustomSpace1 : public CustomSpace<CustomSpace1> {
 public:
  static constexpr size_t kSpaceIndex = 0;
};

class CustomSpace2 : public CustomSpace<CustomSpace2> {
 public:
  static constexpr size_t kSpaceIndex = 1;
};

namespace internal {

namespace {

size_t g_destructor_callcount;

class TestWithHeapWithCustomSpaces : public testing::TestWithPlatform {
 protected:
  TestWithHeapWithCustomSpaces() {
    Heap::HeapOptions options;
    options.custom_spaces.emplace_back(std::make_unique<CustomSpace1>());
    options.custom_spaces.emplace_back(std::make_unique<CustomSpace2>());
    heap_ = Heap::Create(platform_, std::move(options));
    g_destructor_callcount = 0;
  }

  void PreciseGC() {
    heap_->ForceGarbageCollectionSlow(
        "TestWithHeapWithCustomSpaces", "Testing",
        Heap::GCConfig::StackState::kNoHeapPointers);
  }

  cppgc::Heap* GetHeap() const { return heap_.get(); }

 private:
  std::unique_ptr<cppgc::Heap> heap_;
};

class RegularGCed final : public GarbageCollected<RegularGCed> {
 public:
  void Trace(Visitor*) const {}
};

class CustomGCed1 final : public GarbageCollected<CustomGCed1> {
 public:
  ~CustomGCed1() { g_destructor_callcount++; }
  void Trace(Visitor*) const {}
};
class CustomGCed2 final : public GarbageCollected<CustomGCed2> {
 public:
  ~CustomGCed2() { g_destructor_callcount++; }
  void Trace(Visitor*) const {}
};

class CustomGCedBase : public GarbageCollected<CustomGCedBase> {
 public:
  void Trace(Visitor*) const {}
};
class CustomGCedFinal1 final : public CustomGCedBase {
 public:
  ~CustomGCedFinal1() { g_destructor_callcount++; }
};
class CustomGCedFinal2 final : public CustomGCedBase {
 public:
  ~CustomGCedFinal2() { g_destructor_callcount++; }
};

}  // namespace

}  // namespace internal

template <>
struct SpaceTrait<internal::CustomGCed1> {
  using Space = CustomSpace1;
};

template <>
struct SpaceTrait<internal::CustomGCed2> {
  using Space = CustomSpace2;
};

template <typename T>
struct SpaceTrait<
    T, std::enable_if_t<std::is_base_of<internal::CustomGCedBase, T>::value>> {
  using Space = CustomSpace1;
};

namespace internal {

TEST_F(TestWithHeapWithCustomSpaces, AllocateOnCustomSpaces) {
  auto* regular = MakeGarbageCollected<RegularGCed>(GetHeap());
  auto* custom1 = MakeGarbageCollected<CustomGCed1>(GetHeap());
  auto* custom2 = MakeGarbageCollected<CustomGCed2>(GetHeap());
  EXPECT_EQ(RawHeap::kNumberOfRegularSpaces,
            NormalPage::FromPayload(custom1)->space()->index());
  EXPECT_EQ(RawHeap::kNumberOfRegularSpaces + 1,
            NormalPage::FromPayload(custom2)->space()->index());
  EXPECT_EQ(static_cast<size_t>(RawHeap::RegularSpaceType::kNormal1),
            NormalPage::FromPayload(regular)->space()->index());
}

TEST_F(TestWithHeapWithCustomSpaces,
       AllocateOnCustomSpacesSpecifiedThroughBase) {
  auto* regular = MakeGarbageCollected<RegularGCed>(GetHeap());
  auto* custom1 = MakeGarbageCollected<CustomGCedFinal1>(GetHeap());
  auto* custom2 = MakeGarbageCollected<CustomGCedFinal2>(GetHeap());
  EXPECT_EQ(RawHeap::kNumberOfRegularSpaces,
            NormalPage::FromPayload(custom1)->space()->index());
  EXPECT_EQ(RawHeap::kNumberOfRegularSpaces,
            NormalPage::FromPayload(custom2)->space()->index());
  EXPECT_EQ(static_cast<size_t>(RawHeap::RegularSpaceType::kNormal1),
            NormalPage::FromPayload(regular)->space()->index());
}

TEST_F(TestWithHeapWithCustomSpaces, SweepCustomSpace) {
  MakeGarbageCollected<CustomGCedFinal1>(GetHeap());
  MakeGarbageCollected<CustomGCedFinal2>(GetHeap());
  MakeGarbageCollected<CustomGCed1>(GetHeap());
  MakeGarbageCollected<CustomGCed2>(GetHeap());
  EXPECT_EQ(0u, g_destructor_callcount);
  PreciseGC();
  EXPECT_EQ(4u, g_destructor_callcount);
}

}  // namespace internal
}  // namespace cppgc
