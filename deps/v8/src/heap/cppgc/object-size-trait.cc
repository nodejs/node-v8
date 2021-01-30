// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/cppgc/object-size-trait.h"

#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/heap-page.h"

namespace cppgc {
namespace internal {

// static
size_t BaseObjectSizeTrait::GetObjectSizeForGarbageCollected(
    const void* object) {
  const auto& header = HeapObjectHeader::FromPayload(object);
  return header.IsLargeObject()
             ? static_cast<const LargePage*>(BasePage::FromPayload(&header))
                   ->PayloadSize()
             : header.GetSize();
}

// static
size_t BaseObjectSizeTrait::GetObjectSizeForGarbageCollectedMixin(
    const void* address) {
  // `address` is guaranteed to be on a normal page because large object mixins
  // are not supported.
  const auto& header =
      BasePage::FromPayload(address)
          ->ObjectHeaderFromInnerAddress<AccessMode::kAtomic>(address);
  DCHECK(!header.IsLargeObject());
  return header.GetSize();
}

}  // namespace internal
}  // namespace cppgc
