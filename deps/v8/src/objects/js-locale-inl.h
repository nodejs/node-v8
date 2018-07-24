// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTL_SUPPORT
#error Internationalization is expected to be enabled.
#endif  // V8_INTL_SUPPORT

#ifndef V8_OBJECTS_JS_LOCALE_INL_H_
#define V8_OBJECTS_JS_LOCALE_INL_H_

#include "src/api-inl.h"
#include "src/objects-inl.h"
#include "src/objects/js-locale.h"

// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

// Base locale accessors.
ACCESSORS(JSLocale, language, Object, kLanguageOffset);
ACCESSORS(JSLocale, script, Object, kScriptOffset);
ACCESSORS(JSLocale, region, Object, kRegionOffset);
ACCESSORS(JSLocale, base_name, Object, kBaseNameOffset);
ACCESSORS(JSLocale, locale, String, kLocaleOffset);

// Unicode extension accessors.
ACCESSORS(JSLocale, calendar, Object, kCalendarOffset);
ACCESSORS(JSLocale, case_first, Object, kCaseFirstOffset);
ACCESSORS(JSLocale, collation, Object, kCollationOffset);
ACCESSORS(JSLocale, hour_cycle, Object, kHourCycleOffset);
ACCESSORS(JSLocale, numeric, Object, kNumericOffset);
ACCESSORS(JSLocale, numbering_system, Object, kNumberingSystemOffset);

CAST_ACCESSOR(JSLocale);

}  // namespace internal
}  // namespace v8

#endif  // V8_OBJECTS_JS_LOCALE_INL_H_
