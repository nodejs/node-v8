// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTL_SUPPORT
#error Internationalization is expected to be enabled.
#endif  // V8_INTL_SUPPORT

#include "src/objects/js-segment-iterator.h"

#include <map>
#include <memory>
#include <string>

#include "src/execution/isolate.h"
#include "src/heap/factory.h"
#include "src/objects/intl-objects.h"
#include "src/objects/js-segment-iterator-inl.h"
#include "src/objects/js-segments.h"
#include "src/objects/managed-inl.h"
#include "src/objects/objects-inl.h"
#include "unicode/brkiter.h"

namespace v8 {
namespace internal {

Handle<String> JSSegmentIterator::GranularityAsString(Isolate* isolate) const {
  return JSSegmenter::GetGranularityString(isolate, granularity());
}

// ecma402 #sec-createsegmentiterator
MaybeHandle<JSSegmentIterator> JSSegmentIterator::Create(
    Isolate* isolate, Handle<String> input_string,
    icu::BreakIterator* break_iterator, JSSegmenter::Granularity granularity) {
  // Clone a copy for both the ownership and not sharing with containing and
  // other calls to the iterator because icu::BreakIterator keep the iteration
  // position internally and cannot be shared across multiple calls to
  // JSSegmentIterator::Create and JSSegments::Containing.
  break_iterator = break_iterator->clone();
  DCHECK_NOT_NULL(break_iterator);
  Handle<Map> map = Handle<Map>(
      isolate->native_context()->intl_segment_iterator_map(), isolate);

  // 5. Set iterator.[[IteratedStringNextSegmentCodeUnitIndex]] to 0.
  break_iterator->first();
  Handle<Managed<icu::BreakIterator>> managed_break_iterator =
      Managed<icu::BreakIterator>::FromRawPtr(isolate, 0, break_iterator);

  icu::UnicodeString* string = new icu::UnicodeString();
  break_iterator->getText().getText(*string);
  Handle<Managed<icu::UnicodeString>> unicode_string =
      Managed<icu::UnicodeString>::FromRawPtr(isolate, 0, string);

  break_iterator->setText(*string);

  // Now all properties are ready, so we can allocate the result object.
  Handle<JSObject> result = isolate->factory()->NewJSObjectFromMap(map);
  DisallowGarbageCollection no_gc;
  Handle<JSSegmentIterator> segment_iterator =
      Handle<JSSegmentIterator>::cast(result);

  segment_iterator->set_flags(0);
  segment_iterator->set_granularity(granularity);
  segment_iterator->set_icu_break_iterator(*managed_break_iterator);
  segment_iterator->set_raw_string(*input_string);
  segment_iterator->set_unicode_string(*unicode_string);

  return segment_iterator;
}

// ecma402 #sec-%segmentiteratorprototype%.next
MaybeHandle<JSReceiver> JSSegmentIterator::Next(
    Isolate* isolate, Handle<JSSegmentIterator> segment_iterator) {
  // Sketches of ideas for future performance improvements, roughly in order
  // of difficulty:
  // - Add a fast path for grapheme segmentation of one-byte strings that
  //   entirely skips calling into ICU.
  // - When we enter this function, perform a batch of calls into ICU and
  //   stash away the results, so the next couple of invocations can access
  //   them from a (Torque?) builtin without calling into C++.
  // - Implement compiler support for escape-analyzing the JSSegmentDataObject
  //   and avoid allocating it when possible.

  // TODO(v8:14681): We StackCheck here to break execution in the event of an
  // interrupt. Ordinarily in JS loops, this stack check should already be
  // occuring, however some loops implemented within CodeStubAssembler and
  // Torque builtins do not currently implement these checks. A preferable
  // solution which would benefit other iterators implemented in C++ include:
  //   1) Performing the stack check in CEntry, which would provide a solution
  //   for all methods implemented in C++.
  //
  //   2) Rewriting the loop to include an outer loop, which performs periodic
  //   stack checks every N loop bodies (where N is some arbitrary heuristic
  //   selected to allow short loop counts to run with few interruptions).
  STACK_CHECK(isolate, MaybeHandle<JSReceiver>());

  Factory* factory = isolate->factory();
  icu::BreakIterator* icu_break_iterator =
      segment_iterator->icu_break_iterator()->raw();
  // 5. Let startIndex be iterator.[[IteratedStringNextSegmentCodeUnitIndex]].
  int32_t start_index = icu_break_iterator->current();
  // 6. Let endIndex be ! FindBoundary(segmenter, string, startIndex, after).
  int32_t end_index = icu_break_iterator->next();

  // 7. If endIndex is not finite, then
  if (end_index == icu::BreakIterator::DONE) {
    // a. Return ! CreateIterResultObject(undefined, true).
    return factory->NewJSIteratorResult(isolate->factory()->undefined_value(),
                                        true);
  }

  // 8. Set iterator.[[IteratedStringNextSegmentCodeUnitIndex]] to endIndex.

  // 9. Let segmentData be ! CreateSegmentDataObject(segmenter, string,
  // startIndex, endIndex).

  Handle<JSSegmentDataObject> segment_data;
  if (segment_iterator->granularity() == JSSegmenter::Granularity::GRAPHEME &&
      start_index == end_index - 1) {
    // Fast path: use cached segment string and skip avoidable handle creations.
    Handle<String> segment;
    uint16_t code = segment_iterator->raw_string()->Get(start_index);
    if (code > unibrow::Latin1::kMaxChar) {
      segment = factory->LookupSingleCharacterStringFromCode(code);
    }
    Handle<Object> index;
    if (!Smi::IsValid(start_index)) index = factory->NewHeapNumber(start_index);
    Handle<Map> map(isolate->native_context()->intl_segment_data_object_map(),
                    isolate);
    segment_data =
        Handle<JSSegmentDataObject>::cast(factory->NewJSObjectFromMap(map));
    Tagged<JSSegmentDataObject> raw = *segment_data;
    DisallowHeapAllocation no_gc;
    // We can skip write barriers because {segment_data} is the last object
    // that was allocated.
    raw->set_segment(
        code <= unibrow::Latin1::kMaxChar
            ? String::cast(factory->single_character_string_table()->get(code))
            : *segment,
        SKIP_WRITE_BARRIER);
    raw->set_index(
        Smi::IsValid(start_index) ? Smi::FromInt(start_index) : *index,
        SKIP_WRITE_BARRIER);
    raw->set_input(segment_iterator->raw_string(), SKIP_WRITE_BARRIER);
  } else {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, segment_data,
        JSSegments::CreateSegmentDataObject(
            isolate, segment_iterator->granularity(), icu_break_iterator,
            handle(segment_iterator->raw_string(), isolate),
            *segment_iterator->unicode_string()->raw(), start_index, end_index),
        JSReceiver);
  }

  // 10. Return ! CreateIterResultObject(segmentData, false).
  return factory->NewJSIteratorResult(segment_data, false);
}

}  // namespace internal
}  // namespace v8
