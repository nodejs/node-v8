// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CRDTP_JSON_H_
#define V8_CRDTP_JSON_H_

#include <memory>
#include "export.h"
#include "parser_handler.h"

namespace v8_crdtp {
namespace json {
// =============================================================================
// json::NewJSONEncoder - for encoding streaming parser events as JSON
// =============================================================================

// Returns a handler object which will write ascii characters to |out|.
// |status->ok()| will be false iff the handler routine HandleError() is called.
// In that case, we'll stop emitting output.
// Except for calling the HandleError routine at any time, the client
// code must call the Handle* methods in an order in which they'd occur
// in valid JSON; otherwise we may crash (the code uses assert).
std::unique_ptr<ParserHandler> NewJSONEncoder(std::vector<uint8_t>* out,
                                              Status* status);

std::unique_ptr<ParserHandler> NewJSONEncoder(std::string* out, Status* status);

// =============================================================================
// json::ParseJSON - for receiving streaming parser events for JSON
// =============================================================================

void ParseJSON(span<uint8_t> chars, ParserHandler* handler);

void ParseJSON(span<uint16_t> chars, ParserHandler* handler);

// =============================================================================
// json::ConvertCBORToJSON, json::ConvertJSONToCBOR - for transcoding
// =============================================================================

Status ConvertCBORToJSON(span<uint8_t> cbor, std::string* json);

Status ConvertCBORToJSON(span<uint8_t> cbor, std::vector<uint8_t>* json);

Status ConvertJSONToCBOR(span<uint8_t> json, std::vector<uint8_t>* cbor);

Status ConvertJSONToCBOR(span<uint16_t> json, std::vector<uint8_t>* cbor);
}  // namespace json
}  // namespace v8_crdtp

#endif  // V8_CRDTP_JSON_H_
