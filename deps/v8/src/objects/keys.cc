// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/keys.h"

#include "src/api/api-arguments-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/handles/handles-inl.h"
#include "src/heap/factory.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/elements-inl.h"
#include "src/objects/field-index-inl.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/module-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/ordered-hash-table-inl.h"
#include "src/objects/property-descriptor.h"
#include "src/objects/prototype.h"
#include "src/utils/identity-map.h"

namespace v8 {
namespace internal {

#define RETURN_NOTHING_IF_NOT_SUCCESSFUL(call) \
  do {                                         \
    if (!(call)) return Nothing<bool>();       \
  } while (false)

#define RETURN_FAILURE_IF_NOT_SUCCESSFUL(call)          \
  do {                                                  \
    ExceptionStatus status_enum_result = (call);        \
    if (!status_enum_result) return status_enum_result; \
  } while (false)

namespace {

static bool ContainsOnlyValidKeys(Handle<FixedArray> array) {
  int len = array->length();
  for (int i = 0; i < len; i++) {
    Object e = array->get(i);
    if (!(e.IsName() || e.IsNumber())) return false;
  }
  return true;
}

static int AddKey(Object key, Handle<FixedArray> combined_keys,
                  Handle<DescriptorArray> descs, int nof_descriptors,
                  int target) {
  for (InternalIndex i : InternalIndex::Range(nof_descriptors)) {
    if (descs->GetKey(i) == key) return 0;
  }
  combined_keys->set(target, key);
  return 1;
}

static Handle<FixedArray> CombineKeys(Isolate* isolate,
                                      Handle<FixedArray> own_keys,
                                      Handle<FixedArray> prototype_chain_keys,
                                      Handle<JSReceiver> receiver,
                                      bool may_have_elements) {
  int prototype_chain_keys_length = prototype_chain_keys->length();
  if (prototype_chain_keys_length == 0) return own_keys;

  Map map = receiver->map();
  int nof_descriptors = map.NumberOfOwnDescriptors();
  if (nof_descriptors == 0 && !may_have_elements) return prototype_chain_keys;

  Handle<DescriptorArray> descs(map.instance_descriptors(), isolate);
  int own_keys_length = own_keys.is_null() ? 0 : own_keys->length();
  Handle<FixedArray> combined_keys = isolate->factory()->NewFixedArray(
      own_keys_length + prototype_chain_keys_length);
  if (own_keys_length != 0) {
    own_keys->CopyTo(0, *combined_keys, 0, own_keys_length);
  }
  int target_keys_length = own_keys_length;
  for (int i = 0; i < prototype_chain_keys_length; i++) {
    target_keys_length += AddKey(prototype_chain_keys->get(i), combined_keys,
                                 descs, nof_descriptors, target_keys_length);
  }
  return FixedArray::ShrinkOrEmpty(isolate, combined_keys, target_keys_length);
}

}  // namespace

// static
MaybeHandle<FixedArray> KeyAccumulator::GetKeys(
    Handle<JSReceiver> object, KeyCollectionMode mode, PropertyFilter filter,
    GetKeysConversion keys_conversion, bool is_for_in, bool skip_indices) {
  Isolate* isolate = object->GetIsolate();
  FastKeyAccumulator accumulator(isolate, object, mode, filter, is_for_in,
                                 skip_indices);
  return accumulator.GetKeys(keys_conversion);
}

Handle<FixedArray> KeyAccumulator::GetKeys(GetKeysConversion convert) {
  if (keys_.is_null()) {
    return isolate_->factory()->empty_fixed_array();
  }
  if (mode_ == KeyCollectionMode::kOwnOnly &&
      keys_->map() == ReadOnlyRoots(isolate_).fixed_array_map()) {
    return Handle<FixedArray>::cast(keys_);
  }
  USE(ContainsOnlyValidKeys);
  Handle<FixedArray> result =
      OrderedHashSet::ConvertToKeysArray(isolate(), keys(), convert);
  DCHECK(ContainsOnlyValidKeys(result));

  if (try_prototype_info_cache_ && !first_prototype_map_.is_null()) {
    PrototypeInfo::cast(first_prototype_map_->prototype_info())
        .set_prototype_chain_enum_cache(*result);
    Map::GetOrCreatePrototypeChainValidityCell(
        Handle<Map>(receiver_->map(), isolate_), isolate_);
    DCHECK(first_prototype_map_->IsPrototypeValidityCellValid());
  }
  return result;
}

Handle<OrderedHashSet> KeyAccumulator::keys() {
  return Handle<OrderedHashSet>::cast(keys_);
}

ExceptionStatus KeyAccumulator::AddKey(Object key, AddKeyConversion convert) {
  return AddKey(handle(key, isolate_), convert);
}

ExceptionStatus KeyAccumulator::AddKey(Handle<Object> key,
                                       AddKeyConversion convert) {
  if (filter_ == PRIVATE_NAMES_ONLY) {
    if (!key->IsSymbol()) return ExceptionStatus::kSuccess;
    if (!Symbol::cast(*key).is_private_name()) return ExceptionStatus::kSuccess;
  } else if (key->IsSymbol()) {
    if (filter_ & SKIP_SYMBOLS) return ExceptionStatus::kSuccess;
    if (Symbol::cast(*key).is_private()) return ExceptionStatus::kSuccess;
  } else if (filter_ & SKIP_STRINGS) {
    return ExceptionStatus::kSuccess;
  }

  if (IsShadowed(key)) return ExceptionStatus::kSuccess;
  if (keys_.is_null()) {
    keys_ = OrderedHashSet::Allocate(isolate_, 16).ToHandleChecked();
  }
  uint32_t index;
  if (convert == CONVERT_TO_ARRAY_INDEX && key->IsString() &&
      Handle<String>::cast(key)->AsArrayIndex(&index)) {
    key = isolate_->factory()->NewNumberFromUint(index);
  }
  MaybeHandle<OrderedHashSet> new_set_candidate =
      OrderedHashSet::Add(isolate(), keys(), key);
  Handle<OrderedHashSet> new_set;
  if (!new_set_candidate.ToHandle(&new_set)) {
    THROW_NEW_ERROR_RETURN_VALUE(
        isolate_, NewRangeError(MessageTemplate::kTooManyProperties),
        ExceptionStatus::kException);
  }
  if (*new_set != *keys_) {
    // The keys_ Set is converted directly to a FixedArray in GetKeys which can
    // be left-trimmer. Hence the previous Set should not keep a pointer to the
    // new one.
    keys_->set(OrderedHashSet::NextTableIndex(), Smi::zero());
    keys_ = new_set;
  }
  return ExceptionStatus::kSuccess;
}

ExceptionStatus KeyAccumulator::AddKeys(Handle<FixedArray> array,
                                        AddKeyConversion convert) {
  int add_length = array->length();
  for (int i = 0; i < add_length; i++) {
    Handle<Object> current(array->get(i), isolate_);
    RETURN_FAILURE_IF_NOT_SUCCESSFUL(AddKey(current, convert));
  }
  return ExceptionStatus::kSuccess;
}

ExceptionStatus KeyAccumulator::AddKeys(Handle<JSObject> array_like,
                                        AddKeyConversion convert) {
  DCHECK(array_like->IsJSArray() || array_like->HasSloppyArgumentsElements());
  ElementsAccessor* accessor = array_like->GetElementsAccessor();
  return accessor->AddElementsToKeyAccumulator(array_like, this, convert);
}

MaybeHandle<FixedArray> FilterProxyKeys(KeyAccumulator* accumulator,
                                        Handle<JSProxy> owner,
                                        Handle<FixedArray> keys,
                                        PropertyFilter filter) {
  if (filter == ALL_PROPERTIES) {
    // Nothing to do.
    return keys;
  }
  Isolate* isolate = accumulator->isolate();
  int store_position = 0;
  for (int i = 0; i < keys->length(); ++i) {
    Handle<Name> key(Name::cast(keys->get(i)), isolate);
    if (key->FilterKey(filter)) continue;  // Skip this key.
    if (filter & ONLY_ENUMERABLE) {
      PropertyDescriptor desc;
      Maybe<bool> found =
          JSProxy::GetOwnPropertyDescriptor(isolate, owner, key, &desc);
      MAYBE_RETURN(found, MaybeHandle<FixedArray>());
      if (!found.FromJust()) continue;
      if (!desc.enumerable()) {
        accumulator->AddShadowingKey(key);
        continue;
      }
    }
    // Keep this key.
    if (store_position != i) {
      keys->set(store_position, *key);
    }
    store_position++;
  }
  return FixedArray::ShrinkOrEmpty(isolate, keys, store_position);
}

// Returns "nothing" in case of exception, "true" on success.
Maybe<bool> KeyAccumulator::AddKeysFromJSProxy(Handle<JSProxy> proxy,
                                               Handle<FixedArray> keys) {
  // Postpone the enumerable check for for-in to the ForInFilter step.
  if (!is_for_in_) {
    ASSIGN_RETURN_ON_EXCEPTION_VALUE(
        isolate_, keys, FilterProxyKeys(this, proxy, keys, filter_),
        Nothing<bool>());
    if (mode_ == KeyCollectionMode::kOwnOnly) {
      // If we collect only the keys from a JSProxy do not sort or deduplicate.
      keys_ = keys;
      return Just(true);
    }
  }
  RETURN_NOTHING_IF_NOT_SUCCESSFUL(
      AddKeys(keys, is_for_in_ ? CONVERT_TO_ARRAY_INDEX : DO_NOT_CONVERT));
  return Just(true);
}

Maybe<bool> KeyAccumulator::CollectKeys(Handle<JSReceiver> receiver,
                                        Handle<JSReceiver> object) {
  // Proxies have no hidden prototype and we should not trigger the
  // [[GetPrototypeOf]] trap on the last iteration when using
  // AdvanceFollowingProxies.
  if (mode_ == KeyCollectionMode::kOwnOnly && object->IsJSProxy()) {
    MAYBE_RETURN(CollectOwnJSProxyKeys(receiver, Handle<JSProxy>::cast(object)),
                 Nothing<bool>());
    return Just(true);
  }

  PrototypeIterator::WhereToEnd end = mode_ == KeyCollectionMode::kOwnOnly
                                          ? PrototypeIterator::END_AT_NON_HIDDEN
                                          : PrototypeIterator::END_AT_NULL;
  for (PrototypeIterator iter(isolate_, object, kStartAtReceiver, end);
       !iter.IsAtEnd();) {
    // Start the shadow checks only after the first prototype has added
    // shadowing keys.
    if (HasShadowingKeys()) skip_shadow_check_ = false;
    Handle<JSReceiver> current =
        PrototypeIterator::GetCurrent<JSReceiver>(iter);
    Maybe<bool> result = Just(false);  // Dummy initialization.
    if (current->IsJSProxy()) {
      result = CollectOwnJSProxyKeys(receiver, Handle<JSProxy>::cast(current));
    } else {
      DCHECK(current->IsJSObject());
      result = CollectOwnKeys(receiver, Handle<JSObject>::cast(current));
    }
    MAYBE_RETURN(result, Nothing<bool>());
    if (!result.FromJust()) break;  // |false| means "stop iterating".
    // Iterate through proxies but ignore access checks for the ALL_CAN_READ
    // case on API objects for OWN_ONLY keys handled in CollectOwnKeys.
    if (!iter.AdvanceFollowingProxiesIgnoringAccessChecks()) {
      return Nothing<bool>();
    }
    if (!last_non_empty_prototype_.is_null() &&
        *last_non_empty_prototype_ == *current) {
      break;
    }
  }
  return Just(true);
}

bool KeyAccumulator::HasShadowingKeys() { return !shadowing_keys_.is_null(); }

bool KeyAccumulator::IsShadowed(Handle<Object> key) {
  if (!HasShadowingKeys() || skip_shadow_check_) return false;
  return shadowing_keys_->Has(isolate_, key);
}

void KeyAccumulator::AddShadowingKey(Object key,
                                     AllowHeapAllocation* allow_gc) {
  if (mode_ == KeyCollectionMode::kOwnOnly) return;
  AddShadowingKey(handle(key, isolate_));
}
void KeyAccumulator::AddShadowingKey(Handle<Object> key) {
  if (mode_ == KeyCollectionMode::kOwnOnly) return;
  if (shadowing_keys_.is_null()) {
    shadowing_keys_ = ObjectHashSet::New(isolate_, 16);
  }
  shadowing_keys_ = ObjectHashSet::Add(isolate(), shadowing_keys_, key);
}

namespace {

void TrySettingEmptyEnumCache(JSReceiver object) {
  Map map = object.map();
  DCHECK_EQ(kInvalidEnumCacheSentinel, map.EnumLength());
  if (!map.OnlyHasSimpleProperties()) return;
  if (map.IsJSProxyMap()) return;
  if (map.NumberOfEnumerableProperties() > 0) return;
  DCHECK(object.IsJSObject());
  map.SetEnumLength(0);
}

bool CheckAndInitalizeEmptyEnumCache(JSReceiver object) {
  if (object.map().EnumLength() == kInvalidEnumCacheSentinel) {
    TrySettingEmptyEnumCache(object);
  }
  if (object.map().EnumLength() != 0) return false;
  DCHECK(object.IsJSObject());
  return !JSObject::cast(object).HasEnumerableElements();
}
}  // namespace

void FastKeyAccumulator::Prepare() {
  DisallowHeapAllocation no_gc;
  // Directly go for the fast path for OWN_ONLY keys.
  if (mode_ == KeyCollectionMode::kOwnOnly) return;
  // Fully walk the prototype chain and find the last prototype with keys.
  is_receiver_simple_enum_ = false;
  has_empty_prototype_ = true;
  only_own_has_simple_elements_ =
      !receiver_->map().IsCustomElementsReceiverMap();
  JSReceiver last_prototype;
  may_have_elements_ = MayHaveElements(*receiver_);
  for (PrototypeIterator iter(isolate_, *receiver_); !iter.IsAtEnd();
       iter.Advance()) {
    JSReceiver current = iter.GetCurrent<JSReceiver>();
    if (!may_have_elements_ || only_own_has_simple_elements_) {
      if (MayHaveElements(current)) {
        may_have_elements_ = true;
        only_own_has_simple_elements_ = false;
      }
    }
    bool has_no_properties = CheckAndInitalizeEmptyEnumCache(current);
    if (has_no_properties) continue;
    last_prototype = current;
    has_empty_prototype_ = false;
  }
  // Check if we should try to create/use prototype info cache.
  try_prototype_info_cache_ = TryPrototypeInfoCache(receiver_);
  if (has_prototype_info_cache_) return;
  if (has_empty_prototype_) {
    is_receiver_simple_enum_ =
        receiver_->map().EnumLength() != kInvalidEnumCacheSentinel &&
        !JSObject::cast(*receiver_).HasEnumerableElements();
  } else if (!last_prototype.is_null()) {
    last_non_empty_prototype_ = handle(last_prototype, isolate_);
  }
}

namespace {

Handle<FixedArray> ReduceFixedArrayTo(Isolate* isolate,
                                      Handle<FixedArray> array, int length) {
  DCHECK_LE(length, array->length());
  if (array->length() == length) return array;
  return isolate->factory()->CopyFixedArrayUpTo(array, length);
}

// Initializes and directly returns the enume cache. Users of this function
// have to make sure to never directly leak the enum cache.
Handle<FixedArray> GetFastEnumPropertyKeys(Isolate* isolate,
                                           Handle<JSObject> object) {
  Handle<Map> map(object->map(), isolate);
  Handle<FixedArray> keys(map->instance_descriptors().enum_cache().keys(),
                          isolate);

  // Check if the {map} has a valid enum length, which implies that it
  // must have a valid enum cache as well.
  int enum_length = map->EnumLength();
  if (enum_length != kInvalidEnumCacheSentinel) {
    DCHECK(map->OnlyHasSimpleProperties());
    DCHECK_LE(enum_length, keys->length());
    DCHECK_EQ(enum_length, map->NumberOfEnumerableProperties());
    isolate->counters()->enum_cache_hits()->Increment();
    return ReduceFixedArrayTo(isolate, keys, enum_length);
  }

  // Determine the actual number of enumerable properties of the {map}.
  enum_length = map->NumberOfEnumerableProperties();

  // Check if there's already a shared enum cache on the {map}s
  // DescriptorArray with sufficient number of entries.
  if (enum_length <= keys->length()) {
    if (map->OnlyHasSimpleProperties()) map->SetEnumLength(enum_length);
    isolate->counters()->enum_cache_hits()->Increment();
    return ReduceFixedArrayTo(isolate, keys, enum_length);
  }

  Handle<DescriptorArray> descriptors =
      Handle<DescriptorArray>(map->instance_descriptors(), isolate);
  isolate->counters()->enum_cache_misses()->Increment();

  // Create the keys array.
  int index = 0;
  bool fields_only = true;
  keys = isolate->factory()->NewFixedArray(enum_length);
  for (InternalIndex i : map->IterateOwnDescriptors()) {
    DisallowHeapAllocation no_gc;
    PropertyDetails details = descriptors->GetDetails(i);
    if (details.IsDontEnum()) continue;
    Object key = descriptors->GetKey(i);
    if (key.IsSymbol()) continue;
    keys->set(index, key);
    if (details.location() != kField) fields_only = false;
    index++;
  }
  DCHECK_EQ(index, keys->length());

  // Optionally also create the indices array.
  Handle<FixedArray> indices = isolate->factory()->empty_fixed_array();
  if (fields_only) {
    indices = isolate->factory()->NewFixedArray(enum_length);
    index = 0;
    for (InternalIndex i : map->IterateOwnDescriptors()) {
      DisallowHeapAllocation no_gc;
      PropertyDetails details = descriptors->GetDetails(i);
      if (details.IsDontEnum()) continue;
      Object key = descriptors->GetKey(i);
      if (key.IsSymbol()) continue;
      DCHECK_EQ(kData, details.kind());
      DCHECK_EQ(kField, details.location());
      FieldIndex field_index = FieldIndex::ForDescriptor(*map, i);
      indices->set(index, Smi::FromInt(field_index.GetLoadByFieldIndex()));
      index++;
    }
    DCHECK_EQ(index, indices->length());
  }

  DescriptorArray::InitializeOrChangeEnumCache(descriptors, isolate, keys,
                                               indices);
  if (map->OnlyHasSimpleProperties()) map->SetEnumLength(enum_length);

  return keys;
}

template <bool fast_properties>
MaybeHandle<FixedArray> GetOwnKeysWithElements(Isolate* isolate,
                                               Handle<JSObject> object,
                                               GetKeysConversion convert,
                                               bool skip_indices) {
  Handle<FixedArray> keys;
  ElementsAccessor* accessor = object->GetElementsAccessor();
  if (fast_properties) {
    keys = GetFastEnumPropertyKeys(isolate, object);
  } else {
    // TODO(cbruni): preallocate big enough array to also hold elements.
    keys = KeyAccumulator::GetOwnEnumPropertyKeys(isolate, object);
  }

  MaybeHandle<FixedArray> result;
  if (skip_indices) {
    result = keys;
  } else {
    result =
        accessor->PrependElementIndices(object, keys, convert, ONLY_ENUMERABLE);
  }

  if (FLAG_trace_for_in_enumerate) {
    PrintF("| strings=%d symbols=0 elements=%u || prototypes>=1 ||\n",
           keys->length(), result.ToHandleChecked()->length() - keys->length());
  }
  return result;
}

}  // namespace

MaybeHandle<FixedArray> FastKeyAccumulator::GetKeys(
    GetKeysConversion keys_conversion) {
  // TODO(v8:9401): We should extend the fast path of KeyAccumulator::GetKeys to
  // also use fast path even when filter = SKIP_SYMBOLS. We used to pass wrong
  // filter to use fast path in cases where we tried to verify all properties
  // are enumerable. However these checks weren't correct and passing the wrong
  // filter led to wrong behaviour.
  if (filter_ == ENUMERABLE_STRINGS) {
    Handle<FixedArray> keys;
    if (GetKeysFast(keys_conversion).ToHandle(&keys)) {
      return keys;
    }
    if (isolate_->has_pending_exception()) return MaybeHandle<FixedArray>();
  }

  if (try_prototype_info_cache_) {
    return GetKeysWithPrototypeInfoCache(keys_conversion);
  }
  return GetKeysSlow(keys_conversion);
}

MaybeHandle<FixedArray> FastKeyAccumulator::GetKeysFast(
    GetKeysConversion keys_conversion) {
  bool own_only = has_empty_prototype_ || mode_ == KeyCollectionMode::kOwnOnly;
  Map map = receiver_->map();
  if (!own_only || map.IsCustomElementsReceiverMap()) {
    return MaybeHandle<FixedArray>();
  }

  // From this point on we are certain to only collect own keys.
  DCHECK(receiver_->IsJSObject());
  Handle<JSObject> object = Handle<JSObject>::cast(receiver_);

  // Do not try to use the enum-cache for dict-mode objects.
  if (map.is_dictionary_map()) {
    return GetOwnKeysWithElements<false>(isolate_, object, keys_conversion,
                                         skip_indices_);
  }
  int enum_length = receiver_->map().EnumLength();
  if (enum_length == kInvalidEnumCacheSentinel) {
    Handle<FixedArray> keys;
    // Try initializing the enum cache and return own properties.
    if (GetOwnKeysWithUninitializedEnumCache().ToHandle(&keys)) {
      if (FLAG_trace_for_in_enumerate) {
        PrintF("| strings=%d symbols=0 elements=0 || prototypes>=1 ||\n",
               keys->length());
      }
      is_receiver_simple_enum_ =
          object->map().EnumLength() != kInvalidEnumCacheSentinel;
      return keys;
    }
  }
  // The properties-only case failed because there were probably elements on the
  // receiver.
  return GetOwnKeysWithElements<true>(isolate_, object, keys_conversion,
                                      skip_indices_);
}

MaybeHandle<FixedArray>
FastKeyAccumulator::GetOwnKeysWithUninitializedEnumCache() {
  Handle<JSObject> object = Handle<JSObject>::cast(receiver_);
  // Uninitalized enum cache
  Map map = object->map();
  if (object->elements() != ReadOnlyRoots(isolate_).empty_fixed_array() &&
      object->elements() !=
          ReadOnlyRoots(isolate_).empty_slow_element_dictionary()) {
    // Assume that there are elements.
    return MaybeHandle<FixedArray>();
  }
  int number_of_own_descriptors = map.NumberOfOwnDescriptors();
  if (number_of_own_descriptors == 0) {
    map.SetEnumLength(0);
    return isolate_->factory()->empty_fixed_array();
  }
  // We have no elements but possibly enumerable property keys, hence we can
  // directly initialize the enum cache.
  Handle<FixedArray> keys = GetFastEnumPropertyKeys(isolate_, object);
  if (is_for_in_) return keys;
  // Do not leak the enum cache as it might end up as an elements backing store.
  return isolate_->factory()->CopyFixedArray(keys);
}

MaybeHandle<FixedArray> FastKeyAccumulator::GetKeysSlow(
    GetKeysConversion keys_conversion) {
  KeyAccumulator accumulator(isolate_, mode_, filter_);
  accumulator.set_is_for_in(is_for_in_);
  accumulator.set_skip_indices(skip_indices_);
  accumulator.set_last_non_empty_prototype(last_non_empty_prototype_);
  accumulator.set_may_have_elements(may_have_elements_);
  accumulator.set_first_prototype_map(first_prototype_map_);
  accumulator.set_try_prototype_info_cache(try_prototype_info_cache_);

  MAYBE_RETURN(accumulator.CollectKeys(receiver_, receiver_),
               MaybeHandle<FixedArray>());
  return accumulator.GetKeys(keys_conversion);
}

MaybeHandle<FixedArray> FastKeyAccumulator::GetKeysWithPrototypeInfoCache(
    GetKeysConversion keys_conversion) {
  Handle<FixedArray> own_keys;
  if (may_have_elements_) {
    if (receiver_->map().is_dictionary_map()) {
      GetOwnKeysWithElements<false>(isolate_, Handle<JSObject>::cast(receiver_),
                                    keys_conversion, skip_indices_)
          .ToHandle(&own_keys);
    } else {
      GetOwnKeysWithElements<true>(isolate_, Handle<JSObject>::cast(receiver_),
                                   keys_conversion, skip_indices_)
          .ToHandle(&own_keys);
    }
  } else {
    own_keys = KeyAccumulator::GetOwnEnumPropertyKeys(
        isolate_, Handle<JSObject>::cast(receiver_));
  }
  Handle<FixedArray> prototype_chain_keys;
  if (has_prototype_info_cache_) {
    prototype_chain_keys =
        handle(FixedArray::cast(
                   PrototypeInfo::cast(first_prototype_map_->prototype_info())
                       .prototype_chain_enum_cache()),
               isolate_);
  } else {
    KeyAccumulator accumulator(isolate_, mode_, filter_);
    accumulator.set_is_for_in(is_for_in_);
    accumulator.set_skip_indices(skip_indices_);
    accumulator.set_last_non_empty_prototype(last_non_empty_prototype_);
    accumulator.set_may_have_elements(may_have_elements_);
    accumulator.set_receiver(receiver_);
    accumulator.set_first_prototype_map(first_prototype_map_);
    accumulator.set_try_prototype_info_cache(try_prototype_info_cache_);
    MAYBE_RETURN(accumulator.CollectKeys(first_prototype_, first_prototype_),
                 MaybeHandle<FixedArray>());
    prototype_chain_keys = accumulator.GetKeys(keys_conversion);
  }
  Handle<FixedArray> result = CombineKeys(
      isolate_, own_keys, prototype_chain_keys, receiver_, may_have_elements_);
  if (is_for_in_ && own_keys.is_identical_to(result)) {
    // Don't leak the enumeration cache without the receiver since it might get
    // trimmed otherwise.
    return isolate_->factory()->CopyFixedArrayUpTo(result, result->length());
  }
  return result;
}

bool FastKeyAccumulator::MayHaveElements(JSReceiver receiver) {
  if (!receiver.IsJSObject()) return true;
  JSObject object = JSObject::cast(receiver);
  if (object.HasEnumerableElements()) return true;
  if (object.HasIndexedInterceptor()) return true;
  return false;
}

bool FastKeyAccumulator::TryPrototypeInfoCache(Handle<JSReceiver> receiver) {
  if (may_have_elements_ && !only_own_has_simple_elements_) return false;
  Handle<JSObject> object = Handle<JSObject>::cast(receiver);
  if (!object->HasFastProperties()) return false;
  if (object->HasNamedInterceptor()) return false;
  if (object->IsAccessCheckNeeded() &&
      !isolate_->MayAccess(handle(isolate_->context(), isolate_), object)) {
    return false;
  }
  HeapObject prototype = receiver->map().prototype();
  if (prototype.is_null()) return false;
  if (!prototype.map().is_prototype_map() ||
      !prototype.map().prototype_info().IsPrototypeInfo()) {
    return false;
  }
  first_prototype_ = handle(JSReceiver::cast(prototype), isolate_);
  Handle<Map> map(prototype.map(), isolate_);
  first_prototype_map_ = map;
  has_prototype_info_cache_ = map->IsPrototypeValidityCellValid() &&
                              PrototypeInfo::cast(map->prototype_info())
                                  .prototype_chain_enum_cache()
                                  .IsFixedArray();
  return true;
}

namespace {

enum IndexedOrNamed { kIndexed, kNamed };

V8_WARN_UNUSED_RESULT ExceptionStatus FilterForEnumerableProperties(
    Handle<JSReceiver> receiver, Handle<JSObject> object,
    Handle<InterceptorInfo> interceptor, KeyAccumulator* accumulator,
    Handle<JSObject> result, IndexedOrNamed type) {
  DCHECK(result->IsJSArray() || result->HasSloppyArgumentsElements());
  ElementsAccessor* accessor = result->GetElementsAccessor();

  size_t length = accessor->GetCapacity(*result, result->elements());
  for (InternalIndex entry : InternalIndex::Range(length)) {
    if (!accessor->HasEntry(*result, entry)) continue;

    // args are invalid after args.Call(), create a new one in every iteration.
    PropertyCallbackArguments args(accumulator->isolate(), interceptor->data(),
                                   *receiver, *object, Just(kDontThrow));

    Handle<Object> element = accessor->Get(result, entry);
    Handle<Object> attributes;
    if (type == kIndexed) {
      uint32_t number;
      CHECK(element->ToUint32(&number));
      attributes = args.CallIndexedQuery(interceptor, number);
    } else {
      CHECK(element->IsName());
      attributes =
          args.CallNamedQuery(interceptor, Handle<Name>::cast(element));
    }

    if (!attributes.is_null()) {
      int32_t value;
      CHECK(attributes->ToInt32(&value));
      if ((value & DONT_ENUM) == 0) {
        RETURN_FAILURE_IF_NOT_SUCCESSFUL(
            accumulator->AddKey(element, DO_NOT_CONVERT));
      }
    }
  }
  return ExceptionStatus::kSuccess;
}

// Returns |true| on success, |nothing| on exception.
Maybe<bool> CollectInterceptorKeysInternal(Handle<JSReceiver> receiver,
                                           Handle<JSObject> object,
                                           Handle<InterceptorInfo> interceptor,
                                           KeyAccumulator* accumulator,
                                           IndexedOrNamed type) {
  Isolate* isolate = accumulator->isolate();
  PropertyCallbackArguments enum_args(isolate, interceptor->data(), *receiver,
                                      *object, Just(kDontThrow));

  Handle<JSObject> result;
  if (!interceptor->enumerator().IsUndefined(isolate)) {
    if (type == kIndexed) {
      result = enum_args.CallIndexedEnumerator(interceptor);
    } else {
      DCHECK_EQ(type, kNamed);
      result = enum_args.CallNamedEnumerator(interceptor);
    }
  }
  RETURN_VALUE_IF_SCHEDULED_EXCEPTION(isolate, Nothing<bool>());
  if (result.is_null()) return Just(true);

  if ((accumulator->filter() & ONLY_ENUMERABLE) &&
      !interceptor->query().IsUndefined(isolate)) {
    RETURN_NOTHING_IF_NOT_SUCCESSFUL(FilterForEnumerableProperties(
        receiver, object, interceptor, accumulator, result, type));
  } else {
    RETURN_NOTHING_IF_NOT_SUCCESSFUL(accumulator->AddKeys(
        result, type == kIndexed ? CONVERT_TO_ARRAY_INDEX : DO_NOT_CONVERT));
  }
  return Just(true);
}

Maybe<bool> CollectInterceptorKeys(Handle<JSReceiver> receiver,
                                   Handle<JSObject> object,
                                   KeyAccumulator* accumulator,
                                   IndexedOrNamed type) {
  Isolate* isolate = accumulator->isolate();
  if (type == kIndexed) {
    if (!object->HasIndexedInterceptor()) return Just(true);
  } else {
    if (!object->HasNamedInterceptor()) return Just(true);
  }
  Handle<InterceptorInfo> interceptor(type == kIndexed
                                          ? object->GetIndexedInterceptor()
                                          : object->GetNamedInterceptor(),
                                      isolate);
  if ((accumulator->filter() & ONLY_ALL_CAN_READ) &&
      !interceptor->all_can_read()) {
    return Just(true);
  }
  return CollectInterceptorKeysInternal(receiver, object, interceptor,
                                        accumulator, type);
}

}  // namespace

Maybe<bool> KeyAccumulator::CollectOwnElementIndices(
    Handle<JSReceiver> receiver, Handle<JSObject> object) {
  if (filter_ & SKIP_STRINGS || skip_indices_) return Just(true);

  ElementsAccessor* accessor = object->GetElementsAccessor();
  RETURN_NOTHING_IF_NOT_SUCCESSFUL(
      accessor->CollectElementIndices(object, this));
  return CollectInterceptorKeys(receiver, object, this, kIndexed);
}

namespace {

template <bool skip_symbols>
base::Optional<int> CollectOwnPropertyNamesInternal(
    Handle<JSObject> object, KeyAccumulator* keys,
    Handle<DescriptorArray> descs, int start_index, int limit) {
  AllowHeapAllocation allow_gc;
  int first_skipped = -1;
  PropertyFilter filter = keys->filter();
  KeyCollectionMode mode = keys->mode();
  for (InternalIndex i : InternalIndex::Range(start_index, limit)) {
    bool is_shadowing_key = false;
    PropertyDetails details = descs->GetDetails(i);

    if ((details.attributes() & filter) != 0) {
      if (mode == KeyCollectionMode::kIncludePrototypes) {
        is_shadowing_key = true;
      } else {
        continue;
      }
    }

    if (filter & ONLY_ALL_CAN_READ) {
      if (details.kind() != kAccessor) continue;
      Object accessors = descs->GetStrongValue(i);
      if (!accessors.IsAccessorInfo()) continue;
      if (!AccessorInfo::cast(accessors).all_can_read()) continue;
    }

    Name key = descs->GetKey(i);
    if (skip_symbols == key.IsSymbol()) {
      if (first_skipped == -1) first_skipped = i.as_int();
      continue;
    }
    if (key.FilterKey(keys->filter())) continue;

    if (is_shadowing_key) {
      // This might allocate, but {key} is not used afterwards.
      keys->AddShadowingKey(key, &allow_gc);
      continue;
    } else {
      if (keys->AddKey(key, DO_NOT_CONVERT) != ExceptionStatus::kSuccess) {
        return base::Optional<int>();
      }
    }
  }
  return first_skipped;
}

template <class T>
Handle<FixedArray> GetOwnEnumPropertyDictionaryKeys(Isolate* isolate,
                                                    KeyCollectionMode mode,
                                                    KeyAccumulator* accumulator,
                                                    Handle<JSObject> object,
                                                    T raw_dictionary) {
  Handle<T> dictionary(raw_dictionary, isolate);
  if (dictionary->NumberOfElements() == 0) {
    return isolate->factory()->empty_fixed_array();
  }
  int length = dictionary->NumberOfEnumerableProperties();
  Handle<FixedArray> storage = isolate->factory()->NewFixedArray(length);
  T::CopyEnumKeysTo(isolate, dictionary, storage, mode, accumulator);
  return storage;
}
}  // namespace

Maybe<bool> KeyAccumulator::CollectOwnPropertyNames(Handle<JSReceiver> receiver,
                                                    Handle<JSObject> object) {
  if (filter_ == ENUMERABLE_STRINGS) {
    Handle<FixedArray> enum_keys;
    if (object->HasFastProperties()) {
      enum_keys = KeyAccumulator::GetOwnEnumPropertyKeys(isolate_, object);
      // If the number of properties equals the length of enumerable properties
      // we do not have to filter out non-enumerable ones
      Map map = object->map();
      int nof_descriptors = map.NumberOfOwnDescriptors();
      if (enum_keys->length() != nof_descriptors) {
        if (map.prototype(isolate_) != ReadOnlyRoots(isolate_).null_value()) {
          AllowHeapAllocation allow_gc;
          Handle<DescriptorArray> descs =
              Handle<DescriptorArray>(map.instance_descriptors(), isolate_);
          for (InternalIndex i : InternalIndex::Range(nof_descriptors)) {
            PropertyDetails details = descs->GetDetails(i);
            if (!details.IsDontEnum()) continue;
            this->AddShadowingKey(descs->GetKey(i), &allow_gc);
          }
        }
      }
    } else if (object->IsJSGlobalObject()) {
      enum_keys = GetOwnEnumPropertyDictionaryKeys(
          isolate_, mode_, this, object,
          JSGlobalObject::cast(*object).global_dictionary());
    } else {
      enum_keys = GetOwnEnumPropertyDictionaryKeys(
          isolate_, mode_, this, object, object->property_dictionary());
    }
    if (object->IsJSModuleNamespace()) {
      // Simulate [[GetOwnProperty]] for establishing enumerability, which
      // throws for uninitialized exports.
      for (int i = 0, n = enum_keys->length(); i < n; ++i) {
        Handle<String> key(String::cast(enum_keys->get(i)), isolate_);
        if (Handle<JSModuleNamespace>::cast(object)
                ->GetExport(isolate(), key)
                .is_null()) {
          return Nothing<bool>();
        }
      }
    }
    RETURN_NOTHING_IF_NOT_SUCCESSFUL(AddKeys(enum_keys, DO_NOT_CONVERT));
  } else {
    if (object->HasFastProperties()) {
      int limit = object->map().NumberOfOwnDescriptors();
      Handle<DescriptorArray> descs(object->map().instance_descriptors(),
                                    isolate_);
      // First collect the strings,
      base::Optional<int> first_symbol =
          CollectOwnPropertyNamesInternal<true>(object, this, descs, 0, limit);
      // then the symbols.
      RETURN_NOTHING_IF_NOT_SUCCESSFUL(first_symbol);
      if (first_symbol.value() != -1) {
        RETURN_NOTHING_IF_NOT_SUCCESSFUL(CollectOwnPropertyNamesInternal<false>(
            object, this, descs, first_symbol.value(), limit));
      }
    } else if (object->IsJSGlobalObject()) {
      RETURN_NOTHING_IF_NOT_SUCCESSFUL(GlobalDictionary::CollectKeysTo(
          handle(JSGlobalObject::cast(*object).global_dictionary(), isolate_),
          this));
    } else {
      RETURN_NOTHING_IF_NOT_SUCCESSFUL(NameDictionary::CollectKeysTo(
          handle(object->property_dictionary(), isolate_), this));
    }
  }
  // Add the property keys from the interceptor.
  return CollectInterceptorKeys(receiver, object, this, kNamed);
}

ExceptionStatus KeyAccumulator::CollectPrivateNames(Handle<JSReceiver> receiver,
                                                    Handle<JSObject> object) {
  DCHECK_EQ(mode_, KeyCollectionMode::kOwnOnly);
  if (object->HasFastProperties()) {
    int limit = object->map().NumberOfOwnDescriptors();
    Handle<DescriptorArray> descs(object->map().instance_descriptors(),
                                  isolate_);
    CollectOwnPropertyNamesInternal<false>(object, this, descs, 0, limit);
  } else if (object->IsJSGlobalObject()) {
    RETURN_FAILURE_IF_NOT_SUCCESSFUL(GlobalDictionary::CollectKeysTo(
        handle(JSGlobalObject::cast(*object).global_dictionary(), isolate_),
        this));
  } else {
    RETURN_FAILURE_IF_NOT_SUCCESSFUL(NameDictionary::CollectKeysTo(
        handle(object->property_dictionary(), isolate_), this));
  }
  return ExceptionStatus::kSuccess;
}

Maybe<bool> KeyAccumulator::CollectAccessCheckInterceptorKeys(
    Handle<AccessCheckInfo> access_check_info, Handle<JSReceiver> receiver,
    Handle<JSObject> object) {
  if (!skip_indices_) {
    MAYBE_RETURN((CollectInterceptorKeysInternal(
                     receiver, object,
                     handle(InterceptorInfo::cast(
                                access_check_info->indexed_interceptor()),
                            isolate_),
                     this, kIndexed)),
                 Nothing<bool>());
  }
  MAYBE_RETURN(
      (CollectInterceptorKeysInternal(
          receiver, object,
          handle(InterceptorInfo::cast(access_check_info->named_interceptor()),
                 isolate_),
          this, kNamed)),
      Nothing<bool>());
  return Just(true);
}

// Returns |true| on success, |false| if prototype walking should be stopped,
// |nothing| if an exception was thrown.
Maybe<bool> KeyAccumulator::CollectOwnKeys(Handle<JSReceiver> receiver,
                                           Handle<JSObject> object) {
  // Check access rights if required.
  if (object->IsAccessCheckNeeded() &&
      !isolate_->MayAccess(handle(isolate_->context(), isolate_), object)) {
    // The cross-origin spec says that [[Enumerate]] shall return an empty
    // iterator when it doesn't have access...
    if (mode_ == KeyCollectionMode::kIncludePrototypes) {
      return Just(false);
    }
    // ...whereas [[OwnPropertyKeys]] shall return whitelisted properties.
    DCHECK_EQ(KeyCollectionMode::kOwnOnly, mode_);
    Handle<AccessCheckInfo> access_check_info;
    {
      DisallowHeapAllocation no_gc;
      AccessCheckInfo maybe_info = AccessCheckInfo::Get(isolate_, object);
      if (!maybe_info.is_null()) {
        access_check_info = handle(maybe_info, isolate_);
      }
    }
    // We always have both kinds of interceptors or none.
    if (!access_check_info.is_null() &&
        access_check_info->named_interceptor() != Object()) {
      MAYBE_RETURN(CollectAccessCheckInterceptorKeys(access_check_info,
                                                     receiver, object),
                   Nothing<bool>());
      return Just(false);
    }
    filter_ = static_cast<PropertyFilter>(filter_ | ONLY_ALL_CAN_READ);
  }
  if (filter_ & PRIVATE_NAMES_ONLY) {
    RETURN_NOTHING_IF_NOT_SUCCESSFUL(CollectPrivateNames(receiver, object));
    return Just(true);
  }

  if (may_have_elements_) {
    MAYBE_RETURN(CollectOwnElementIndices(receiver, object), Nothing<bool>());
  }
  MAYBE_RETURN(CollectOwnPropertyNames(receiver, object), Nothing<bool>());
  return Just(true);
}

// static
Handle<FixedArray> KeyAccumulator::GetOwnEnumPropertyKeys(
    Isolate* isolate, Handle<JSObject> object) {
  if (object->HasFastProperties()) {
    return GetFastEnumPropertyKeys(isolate, object);
  } else if (object->IsJSGlobalObject()) {
    return GetOwnEnumPropertyDictionaryKeys(
        isolate, KeyCollectionMode::kOwnOnly, nullptr, object,
        JSGlobalObject::cast(*object).global_dictionary());
  } else {
    return GetOwnEnumPropertyDictionaryKeys(
        isolate, KeyCollectionMode::kOwnOnly, nullptr, object,
        object->property_dictionary());
  }
}

namespace {

class NameComparator {
 public:
  explicit NameComparator(Isolate* isolate) : isolate_(isolate) {}

  bool operator()(uint32_t hash1, uint32_t hash2, const Handle<Name>& key1,
                  const Handle<Name>& key2) const {
    return Name::Equals(isolate_, key1, key2);
  }

 private:
  Isolate* isolate_;
};

}  // namespace

// ES6 #sec-proxy-object-internal-methods-and-internal-slots-ownpropertykeys
// Returns |true| on success, |nothing| in case of exception.
Maybe<bool> KeyAccumulator::CollectOwnJSProxyKeys(Handle<JSReceiver> receiver,
                                                  Handle<JSProxy> proxy) {
  STACK_CHECK(isolate_, Nothing<bool>());
  if (filter_ == PRIVATE_NAMES_ONLY) {
    RETURN_NOTHING_IF_NOT_SUCCESSFUL(NameDictionary::CollectKeysTo(
        handle(proxy->property_dictionary(), isolate_), this));
    return Just(true);
  }

  // 1. Let handler be the value of the [[ProxyHandler]] internal slot of O.
  Handle<Object> handler(proxy->handler(), isolate_);
  // 2. If handler is null, throw a TypeError exception.
  // 3. Assert: Type(handler) is Object.
  if (proxy->IsRevoked()) {
    isolate_->Throw(*isolate_->factory()->NewTypeError(
        MessageTemplate::kProxyRevoked, isolate_->factory()->ownKeys_string()));
    return Nothing<bool>();
  }
  // 4. Let target be the value of the [[ProxyTarget]] internal slot of O.
  Handle<JSReceiver> target(JSReceiver::cast(proxy->target()), isolate_);
  // 5. Let trap be ? GetMethod(handler, "ownKeys").
  Handle<Object> trap;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate_, trap,
      Object::GetMethod(Handle<JSReceiver>::cast(handler),
                        isolate_->factory()->ownKeys_string()),
      Nothing<bool>());
  // 6. If trap is undefined, then
  if (trap->IsUndefined(isolate_)) {
    // 6a. Return target.[[OwnPropertyKeys]]().
    return CollectOwnJSProxyTargetKeys(proxy, target);
  }
  // 7. Let trapResultArray be Call(trap, handler, «target»).
  Handle<Object> trap_result_array;
  Handle<Object> args[] = {target};
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate_, trap_result_array,
      Execution::Call(isolate_, trap, handler, arraysize(args), args),
      Nothing<bool>());
  // 8. Let trapResult be ? CreateListFromArrayLike(trapResultArray,
  //    «String, Symbol»).
  Handle<FixedArray> trap_result;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate_, trap_result,
      Object::CreateListFromArrayLike(isolate_, trap_result_array,
                                      ElementTypes::kStringAndSymbol),
      Nothing<bool>());
  // 9. If trapResult contains any duplicate entries, throw a TypeError
  // exception. Combine with step 18
  // 18. Let uncheckedResultKeys be a new List which is a copy of trapResult.
  Zone set_zone(isolate_->allocator(), ZONE_NAME);
  ZoneAllocationPolicy alloc(&set_zone);
  const int kPresent = 1;
  const int kGone = 0;
  base::TemplateHashMapImpl<Handle<Name>, int, NameComparator,
                            ZoneAllocationPolicy>
      unchecked_result_keys(ZoneHashMap::kDefaultHashMapCapacity,
                            NameComparator(isolate_), alloc);
  int unchecked_result_keys_size = 0;
  for (int i = 0; i < trap_result->length(); ++i) {
    Handle<Name> key(Name::cast(trap_result->get(i)), isolate_);
    auto entry = unchecked_result_keys.LookupOrInsert(key, key->Hash(), alloc);
    if (entry->value != kPresent) {
      entry->value = kPresent;
      unchecked_result_keys_size++;
    } else {
      // found dupes, throw exception
      isolate_->Throw(*isolate_->factory()->NewTypeError(
          MessageTemplate::kProxyOwnKeysDuplicateEntries));
      return Nothing<bool>();
    }
  }
  // 10. Let extensibleTarget be ? IsExtensible(target).
  Maybe<bool> maybe_extensible = JSReceiver::IsExtensible(target);
  MAYBE_RETURN(maybe_extensible, Nothing<bool>());
  bool extensible_target = maybe_extensible.FromJust();
  // 11. Let targetKeys be ? target.[[OwnPropertyKeys]]().
  Handle<FixedArray> target_keys;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(isolate_, target_keys,
                                   JSReceiver::OwnPropertyKeys(target),
                                   Nothing<bool>());
  // 12, 13. (Assert)
  // 14. Let targetConfigurableKeys be an empty List.
  // To save memory, we're re-using target_keys and will modify it in-place.
  Handle<FixedArray> target_configurable_keys = target_keys;
  // 15. Let targetNonconfigurableKeys be an empty List.
  Handle<FixedArray> target_nonconfigurable_keys =
      isolate_->factory()->NewFixedArray(target_keys->length());
  int nonconfigurable_keys_length = 0;
  // 16. Repeat, for each element key of targetKeys:
  for (int i = 0; i < target_keys->length(); ++i) {
    // 16a. Let desc be ? target.[[GetOwnProperty]](key).
    PropertyDescriptor desc;
    Maybe<bool> found = JSReceiver::GetOwnPropertyDescriptor(
        isolate_, target, handle(target_keys->get(i), isolate_), &desc);
    MAYBE_RETURN(found, Nothing<bool>());
    // 16b. If desc is not undefined and desc.[[Configurable]] is false, then
    if (found.FromJust() && !desc.configurable()) {
      // 16b i. Append key as an element of targetNonconfigurableKeys.
      target_nonconfigurable_keys->set(nonconfigurable_keys_length,
                                       target_keys->get(i));
      nonconfigurable_keys_length++;
      // The key was moved, null it out in the original list.
      target_keys->set(i, Smi::zero());
    } else {
      // 16c. Else,
      // 16c i. Append key as an element of targetConfigurableKeys.
      // (No-op, just keep it in |target_keys|.)
    }
  }
  // 17. If extensibleTarget is true and targetNonconfigurableKeys is empty,
  //     then:
  if (extensible_target && nonconfigurable_keys_length == 0) {
    // 17a. Return trapResult.
    return AddKeysFromJSProxy(proxy, trap_result);
  }
  // 18. (Done in step 9)
  // 19. Repeat, for each key that is an element of targetNonconfigurableKeys:
  for (int i = 0; i < nonconfigurable_keys_length; ++i) {
    Object raw_key = target_nonconfigurable_keys->get(i);
    Handle<Name> key(Name::cast(raw_key), isolate_);
    // 19a. If key is not an element of uncheckedResultKeys, throw a
    //      TypeError exception.
    auto found = unchecked_result_keys.Lookup(key, key->Hash());
    if (found == nullptr || found->value == kGone) {
      isolate_->Throw(*isolate_->factory()->NewTypeError(
          MessageTemplate::kProxyOwnKeysMissing, key));
      return Nothing<bool>();
    }
    // 19b. Remove key from uncheckedResultKeys.
    found->value = kGone;
    unchecked_result_keys_size--;
  }
  // 20. If extensibleTarget is true, return trapResult.
  if (extensible_target) {
    return AddKeysFromJSProxy(proxy, trap_result);
  }
  // 21. Repeat, for each key that is an element of targetConfigurableKeys:
  for (int i = 0; i < target_configurable_keys->length(); ++i) {
    Object raw_key = target_configurable_keys->get(i);
    if (raw_key.IsSmi()) continue;  // Zapped entry, was nonconfigurable.
    Handle<Name> key(Name::cast(raw_key), isolate_);
    // 21a. If key is not an element of uncheckedResultKeys, throw a
    //      TypeError exception.
    auto found = unchecked_result_keys.Lookup(key, key->Hash());
    if (found == nullptr || found->value == kGone) {
      isolate_->Throw(*isolate_->factory()->NewTypeError(
          MessageTemplate::kProxyOwnKeysMissing, key));
      return Nothing<bool>();
    }
    // 21b. Remove key from uncheckedResultKeys.
    found->value = kGone;
    unchecked_result_keys_size--;
  }
  // 22. If uncheckedResultKeys is not empty, throw a TypeError exception.
  if (unchecked_result_keys_size != 0) {
    DCHECK_GT(unchecked_result_keys_size, 0);
    isolate_->Throw(*isolate_->factory()->NewTypeError(
        MessageTemplate::kProxyOwnKeysNonExtensible));
    return Nothing<bool>();
  }
  // 23. Return trapResult.
  return AddKeysFromJSProxy(proxy, trap_result);
}

Maybe<bool> KeyAccumulator::CollectOwnJSProxyTargetKeys(
    Handle<JSProxy> proxy, Handle<JSReceiver> target) {
  // TODO(cbruni): avoid creating another KeyAccumulator
  Handle<FixedArray> keys;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate_, keys,
      KeyAccumulator::GetKeys(
          target, KeyCollectionMode::kOwnOnly, ALL_PROPERTIES,
          GetKeysConversion::kConvertToString, is_for_in_, skip_indices_),
      Nothing<bool>());
  Maybe<bool> result = AddKeysFromJSProxy(proxy, keys);
  return result;
}

#undef RETURN_NOTHING_IF_NOT_SUCCESSFUL
#undef RETURN_FAILURE_IF_NOT_SUCCESSFUL
}  // namespace internal
}  // namespace v8
