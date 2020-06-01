// Copyright 2014 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/ast/ast-value-factory.h"

#include "src/base/logging.h"
#include "src/common/globals.h"
#include "src/execution/off-thread-isolate.h"
#include "src/heap/factory-inl.h"
#include "src/heap/off-thread-factory-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/objects.h"
#include "src/strings/char-predicates-inl.h"
#include "src/strings/string-hasher.h"
#include "src/utils/utils-inl.h"

namespace v8 {
namespace internal {

namespace {

// For using StringToIndex.
class OneByteStringStream {
 public:
  explicit OneByteStringStream(Vector<const byte> lb) :
      literal_bytes_(lb), pos_(0) {}

  bool HasMore() { return pos_ < literal_bytes_.length(); }
  uint16_t GetNext() { return literal_bytes_[pos_++]; }

 private:
  Vector<const byte> literal_bytes_;
  int pos_;
};

}  // namespace

void AstRawString::Internalize(Isolate* isolate) {
  DCHECK(!has_string_);
  if (literal_bytes_.length() == 0) {
    set_string(isolate->factory()->empty_string());
  } else if (is_one_byte()) {
    OneByteStringKey key(hash_field_, literal_bytes_);
    set_string(isolate->factory()->InternalizeStringWithKey(&key));
  } else {
    TwoByteStringKey key(hash_field_,
                         Vector<const uint16_t>::cast(literal_bytes_));
    set_string(isolate->factory()->InternalizeStringWithKey(&key));
  }
}

void AstRawString::Internalize(OffThreadIsolate* isolate) {
  DCHECK(!has_string_);
  if (literal_bytes_.length() == 0) {
    set_string(isolate->factory()->empty_string());
    return;
  }

  // For the off-thread case, we already de-duplicated the AstRawStrings during
  // construction and don't have access to the main thread string table yet, so
  // we just unconditionally create strings and will internalize them properly
  // during merging.
  Handle<SeqString> string;
  if (is_one_byte()) {
    string = isolate->factory()->NewOneByteInternalizedString(
        Vector<const uint8_t>::cast(literal_bytes_), hash_field());
  } else {
    string = isolate->factory()->NewTwoByteInternalizedString(
        Vector<const uc16>::cast(literal_bytes_), hash_field());
  }
  set_string(string);
}

bool AstRawString::AsArrayIndex(uint32_t* index) const {
  // The StringHasher will set up the hash. Bail out early if we know it
  // can't be convertible to an array index.
  if ((hash_field_ & Name::kIsNotIntegerIndexMask) != 0) return false;
  if (length() <= Name::kMaxCachedArrayIndexLength) {
    *index = Name::ArrayIndexValueBits::decode(hash_field_);
    return true;
  }
  // Might be an index, but too big to cache it. Do the slow conversion. This
  // might fail if the string is outside uint32_t (but within "safe integer")
  // range.
  OneByteStringStream stream(literal_bytes_);
  return StringToIndex(&stream, index);
}

bool AstRawString::IsIntegerIndex() const {
  return (hash_field_ & Name::kIsNotIntegerIndexMask) == 0;
}

bool AstRawString::IsOneByteEqualTo(const char* data) const {
  if (!is_one_byte()) return false;

  size_t length = static_cast<size_t>(literal_bytes_.length());
  if (length != strlen(data)) return false;

  return 0 == strncmp(reinterpret_cast<const char*>(literal_bytes_.begin()),
                      data, length);
}

uint16_t AstRawString::FirstCharacter() const {
  if (is_one_byte()) return literal_bytes_[0];
  const uint16_t* c = reinterpret_cast<const uint16_t*>(literal_bytes_.begin());
  return *c;
}

bool AstRawString::Compare(void* a, void* b) {
  const AstRawString* lhs = static_cast<AstRawString*>(a);
  const AstRawString* rhs = static_cast<AstRawString*>(b);
  DCHECK_EQ(lhs->Hash(), rhs->Hash());

  if (lhs->length() != rhs->length()) return false;
  if (lhs->length() == 0) return true;
  const unsigned char* l = lhs->raw_data();
  const unsigned char* r = rhs->raw_data();
  size_t length = rhs->length();
  if (lhs->is_one_byte()) {
    if (rhs->is_one_byte()) {
      return CompareCharsUnsigned(reinterpret_cast<const uint8_t*>(l),
                                  reinterpret_cast<const uint8_t*>(r),
                                  length) == 0;
    } else {
      return CompareCharsUnsigned(reinterpret_cast<const uint8_t*>(l),
                                  reinterpret_cast<const uint16_t*>(r),
                                  length) == 0;
    }
  } else {
    if (rhs->is_one_byte()) {
      return CompareCharsUnsigned(reinterpret_cast<const uint16_t*>(l),
                                  reinterpret_cast<const uint8_t*>(r),
                                  length) == 0;
    } else {
      return CompareCharsUnsigned(reinterpret_cast<const uint16_t*>(l),
                                  reinterpret_cast<const uint16_t*>(r),
                                  length) == 0;
    }
  }
}

template <typename LocalIsolate>
Handle<String> AstConsString::Allocate(LocalIsolate* isolate) const {
  DCHECK(string_.is_null());

  if (IsEmpty()) {
    return isolate->factory()->empty_string();
  }
  // AstRawStrings are internalized before AstConsStrings are allocated, so
  // AstRawString::string() will just work.
  Handle<String> tmp = segment_.string->string();
  for (AstConsString::Segment* current = segment_.next; current != nullptr;
       current = current->next) {
    tmp = isolate->factory()
              ->NewConsString(current->string->string(), tmp,
                              AllocationType::kOld)
              .ToHandleChecked();
  }
  return tmp;
}
template EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE)
    Handle<String> AstConsString::Allocate<Isolate>(Isolate* isolate) const;
template EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE)
    Handle<String> AstConsString::Allocate<OffThreadIsolate>(
        OffThreadIsolate* isolate) const;

template <typename LocalIsolate>
Handle<String> AstConsString::AllocateFlat(LocalIsolate* isolate) const {
  if (IsEmpty()) {
    return isolate->factory()->empty_string();
  }
  if (!segment_.next) {
    return segment_.string->string();
  }

  int result_length = 0;
  bool is_one_byte = true;
  for (const AstConsString::Segment* current = &segment_; current != nullptr;
       current = current->next) {
    result_length += current->string->length();
    is_one_byte = is_one_byte && current->string->is_one_byte();
  }

  if (is_one_byte) {
    Handle<SeqOneByteString> result =
        isolate->factory()
            ->NewRawOneByteString(result_length, AllocationType::kOld)
            .ToHandleChecked();
    DisallowHeapAllocation no_gc;
    uint8_t* dest = result->GetChars(no_gc) + result_length;
    for (const AstConsString::Segment* current = &segment_; current != nullptr;
         current = current->next) {
      int length = current->string->length();
      dest -= length;
      CopyChars(dest, current->string->raw_data(), length);
    }
    DCHECK_EQ(dest, result->GetChars(no_gc));
    return result;
  }

  Handle<SeqTwoByteString> result =
      isolate->factory()
          ->NewRawTwoByteString(result_length, AllocationType::kOld)
          .ToHandleChecked();
  DisallowHeapAllocation no_gc;
  uint16_t* dest = result->GetChars(no_gc) + result_length;
  for (const AstConsString::Segment* current = &segment_; current != nullptr;
       current = current->next) {
    int length = current->string->length();
    dest -= length;
    if (current->string->is_one_byte()) {
      CopyChars(dest, current->string->raw_data(), length);
    } else {
      CopyChars(dest,
                reinterpret_cast<const uint16_t*>(current->string->raw_data()),
                length);
    }
  }
  DCHECK_EQ(dest, result->GetChars(no_gc));
  return result;
}
template EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE)
    Handle<String> AstConsString::AllocateFlat<Isolate>(Isolate* isolate) const;
template EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE)
    Handle<String> AstConsString::AllocateFlat<OffThreadIsolate>(
        OffThreadIsolate* isolate) const;

std::forward_list<const AstRawString*> AstConsString::ToRawStrings() const {
  std::forward_list<const AstRawString*> result;
  if (IsEmpty()) {
    return result;
  }

  result.emplace_front(segment_.string);
  for (AstConsString::Segment* current = segment_.next; current != nullptr;
       current = current->next) {
    result.emplace_front(current->string);
  }
  return result;
}

AstStringConstants::AstStringConstants(Isolate* isolate, uint64_t hash_seed)
    : zone_(isolate->allocator(), ZONE_NAME),
      string_table_(AstRawString::Compare),
      hash_seed_(hash_seed) {
  DCHECK_EQ(ThreadId::Current(), isolate->thread_id());
#define F(name, str)                                                       \
  {                                                                        \
    const char* data = str;                                                \
    Vector<const uint8_t> literal(reinterpret_cast<const uint8_t*>(data),  \
                                  static_cast<int>(strlen(data)));         \
    uint32_t hash_field = StringHasher::HashSequentialString<uint8_t>(     \
        literal.begin(), literal.length(), hash_seed_);                    \
    name##_string_ = new (&zone_) AstRawString(true, literal, hash_field); \
    /* The Handle returned by the factory is located on the roots */       \
    /* array, not on the temporary HandleScope, so this is safe.  */       \
    name##_string_->set_string(isolate->factory()->name##_string());       \
    base::HashMap::Entry* entry =                                          \
        string_table_.InsertNew(name##_string_, name##_string_->Hash());   \
    DCHECK_NULL(entry->value);                                             \
    entry->value = reinterpret_cast<void*>(1);                             \
  }
  AST_STRING_CONSTANTS(F)
#undef F
}

AstRawString* AstValueFactory::GetOneByteStringInternal(
    Vector<const uint8_t> literal) {
  if (literal.length() == 1 && literal[0] < kMaxOneCharStringValue) {
    int key = literal[0];
    if (V8_UNLIKELY(one_character_strings_[key] == nullptr)) {
      uint32_t hash_field = StringHasher::HashSequentialString<uint8_t>(
          literal.begin(), literal.length(), hash_seed_);
      one_character_strings_[key] = GetString(hash_field, true, literal);
    }
    return one_character_strings_[key];
  }
  uint32_t hash_field = StringHasher::HashSequentialString<uint8_t>(
      literal.begin(), literal.length(), hash_seed_);
  return GetString(hash_field, true, literal);
}

AstRawString* AstValueFactory::GetTwoByteStringInternal(
    Vector<const uint16_t> literal) {
  uint32_t hash_field = StringHasher::HashSequentialString<uint16_t>(
      literal.begin(), literal.length(), hash_seed_);
  return GetString(hash_field, false, Vector<const byte>::cast(literal));
}

const AstRawString* AstValueFactory::GetString(Handle<String> literal) {
  AstRawString* result = nullptr;
  DisallowHeapAllocation no_gc;
  String::FlatContent content = literal->GetFlatContent(no_gc);
  if (content.IsOneByte()) {
    result = GetOneByteStringInternal(content.ToOneByteVector());
  } else {
    DCHECK(content.IsTwoByte());
    result = GetTwoByteStringInternal(content.ToUC16Vector());
  }
  return result;
}

const AstRawString* AstValueFactory::CloneFromOtherFactory(
    const AstRawString* raw_string) {
  const AstRawString* result = GetString(
      raw_string->hash_field(), raw_string->is_one_byte(),
      Vector<const byte>(raw_string->raw_data(), raw_string->byte_length()));
  return result;
}

AstConsString* AstValueFactory::NewConsString() {
  return new (zone()) AstConsString;
}

AstConsString* AstValueFactory::NewConsString(const AstRawString* str) {
  return NewConsString()->AddString(zone(), str);
}

AstConsString* AstValueFactory::NewConsString(const AstRawString* str1,
                                              const AstRawString* str2) {
  return NewConsString()->AddString(zone(), str1)->AddString(zone(), str2);
}

template <typename LocalIsolate>
void AstValueFactory::Internalize(LocalIsolate* isolate) {
  if (!zone_) return;

  // Strings need to be internalized before values, because values refer to
  // strings.
  for (AstRawString* current = strings_; current != nullptr;) {
    AstRawString* next = current->next();
    current->Internalize(isolate);
    current = next;
  }

  ResetStrings();
  zone_ = nullptr;
}
template EXPORT_TEMPLATE_DEFINE(
    V8_EXPORT_PRIVATE) void AstValueFactory::Internalize<Isolate>(Isolate*
                                                                      isolate);
template EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE) void AstValueFactory::
    Internalize<OffThreadIsolate>(OffThreadIsolate* isolate);

AstRawString* AstValueFactory::GetString(uint32_t hash_field, bool is_one_byte,
                                         Vector<const byte> literal_bytes) {
  // literal_bytes here points to whatever the user passed, and this is OK
  // because we use vector_compare (which checks the contents) to compare
  // against the AstRawStrings which are in the string_table_. We should not
  // return this AstRawString.
  AstRawString key(is_one_byte, literal_bytes, hash_field);
  base::HashMap::Entry* entry = string_table_.LookupOrInsert(&key, key.Hash());
  if (entry->value == nullptr) {
    // Copy literal contents for later comparison.
    int length = literal_bytes.length();
    byte* new_literal_bytes = zone()->NewArray<byte>(length);
    memcpy(new_literal_bytes, literal_bytes.begin(), length);
    AstRawString* new_string = new (zone()) AstRawString(
        is_one_byte, Vector<const byte>(new_literal_bytes, length), hash_field);
    CHECK_NOT_NULL(new_string);
    AddString(new_string);
    entry->key = new_string;
    entry->value = reinterpret_cast<void*>(1);
  }
  return reinterpret_cast<AstRawString*>(entry->key);
}

}  // namespace internal
}  // namespace v8
