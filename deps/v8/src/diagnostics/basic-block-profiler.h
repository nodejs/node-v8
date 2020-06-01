// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_DIAGNOSTICS_BASIC_BLOCK_PROFILER_H_
#define V8_DIAGNOSTICS_BASIC_BLOCK_PROFILER_H_

#include <iosfwd>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "src/base/macros.h"
#include "src/base/platform/mutex.h"
#include "src/common/globals.h"
#include "torque-generated/exported-class-definitions-tq.h"

namespace v8 {
namespace internal {

class BasicBlockProfilerData {
 public:
  explicit BasicBlockProfilerData(size_t n_blocks);
  V8_EXPORT_PRIVATE BasicBlockProfilerData(
      Handle<OnHeapBasicBlockProfilerData> js_heap_data, Isolate* isolate);

  size_t n_blocks() const {
    DCHECK_EQ(block_rpo_numbers_.size(), counts_.size());
    return block_rpo_numbers_.size();
  }
  const uint32_t* counts() const { return &counts_[0]; }

  void SetCode(const std::ostringstream& os);
  void SetFunctionName(std::unique_ptr<char[]> name);
  void SetSchedule(const std::ostringstream& os);
  void SetBlockRpoNumber(size_t offset, int32_t block_rpo);

  // Copy the data from this object into an equivalent object stored on the JS
  // heap, so that it can survive snapshotting and relocation. This must
  // happen on the main thread during finalization of the compilation.
  Handle<OnHeapBasicBlockProfilerData> CopyToJSHeap(Isolate* isolate);

 private:
  friend class BasicBlockProfiler;
  friend std::ostream& operator<<(std::ostream& os,
                                  const BasicBlockProfilerData& s);

  V8_EXPORT_PRIVATE void ResetCounts();

  std::vector<int32_t> block_rpo_numbers_;
  std::vector<uint32_t> counts_;
  std::string function_name_;
  std::string schedule_;
  std::string code_;
  DISALLOW_COPY_AND_ASSIGN(BasicBlockProfilerData);
};

class BasicBlockProfiler {
 public:
  using DataList = std::list<std::unique_ptr<BasicBlockProfilerData>>;

  BasicBlockProfiler() = default;
  ~BasicBlockProfiler() = default;

  V8_EXPORT_PRIVATE static BasicBlockProfiler* Get();
  BasicBlockProfilerData* NewData(size_t n_blocks);
  V8_EXPORT_PRIVATE void ResetCounts(Isolate* isolate);
  V8_EXPORT_PRIVATE void Print(std::ostream& os, Isolate* isolate);

  const DataList* data_list() { return &data_list_; }

 private:
  DataList data_list_;
  base::Mutex data_list_mutex_;

  DISALLOW_COPY_AND_ASSIGN(BasicBlockProfiler);
};

std::ostream& operator<<(std::ostream& os, const BasicBlockProfilerData& s);

}  // namespace internal
}  // namespace v8

#endif  // V8_DIAGNOSTICS_BASIC_BLOCK_PROFILER_H_
