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

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "src/v8.h"

#include "include/libplatform/libplatform.h"
#include "src/api.h"
#include "src/compiler.h"
#include "src/objects-inl.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parsing.h"
#include "src/parsing/preparse-data.h"
#include "src/parsing/preparser.h"
#include "src/parsing/scanner-character-streams.h"
#include "tools/shell-utils.h"

using namespace v8::internal;

class StringResource8 : public v8::String::ExternalOneByteStringResource {
 public:
  StringResource8(const char* data, int length)
      : data_(data), length_(length) { }
  virtual size_t length() const { return length_; }
  virtual const char* data() const { return data_; }

 private:
  const char* data_;
  int length_;
};

v8::base::TimeDelta RunBaselineParser(const char* fname, Encoding encoding,
                                      int repeat, v8::Isolate* isolate,
                                      v8::Local<v8::Context> context) {
  int length = 0;
  const byte* source = ReadFileAndRepeat(fname, &length, repeat);
  v8::Local<v8::String> source_handle;
  switch (encoding) {
    case UTF8: {
      source_handle = v8::String::NewFromUtf8(
                          isolate, reinterpret_cast<const char*>(source),
                          v8::NewStringType::kNormal).ToLocalChecked();
      break;
    }
    case UTF16: {
      source_handle =
          v8::String::NewFromTwoByte(
              isolate, reinterpret_cast<const uint16_t*>(source),
              v8::NewStringType::kNormal, length / 2).ToLocalChecked();
      break;
    }
    case LATIN1: {
      StringResource8* string_resource =
          new StringResource8(reinterpret_cast<const char*>(source), length);
      source_handle = v8::String::NewExternalOneByte(isolate, string_resource)
                          .ToLocalChecked();
      break;
    }
  }
  v8::base::TimeDelta parse_time1;
  Handle<Script> script =
      reinterpret_cast<i::Isolate*>(isolate)->factory()->NewScript(
          v8::Utils::OpenHandle(*source_handle));
  ParseInfo info(script);
  v8::base::ElapsedTimer timer;
  timer.Start();
  bool success =
      parsing::ParseProgram(&info, reinterpret_cast<i::Isolate*>(isolate));
  parse_time1 = timer.Elapsed();
  if (!success) {
    fprintf(stderr, "Parsing failed\n");
    return v8::base::TimeDelta();
  }
  return parse_time1;
}


int main(int argc, char* argv[]) {
  v8::V8::SetFlagsFromCommandLine(&argc, argv, true);
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();
  v8::V8::InitializeExternalStartupData(argv[0]);

  Encoding encoding = LATIN1;
  std::vector<std::string> fnames;
  std::string benchmark;
  int repeat = 1;
  for (int i = 0; i < argc; ++i) {
    if (strcmp(argv[i], "--latin1") == 0) {
      encoding = LATIN1;
    } else if (strcmp(argv[i], "--utf8") == 0) {
      encoding = UTF8;
    } else if (strcmp(argv[i], "--utf16") == 0) {
      encoding = UTF16;
    } else if (strncmp(argv[i], "--benchmark=", 12) == 0) {
      benchmark = std::string(argv[i]).substr(12);
    } else if (strncmp(argv[i], "--repeat=", 9) == 0) {
      std::string repeat_str = std::string(argv[i]).substr(9);
      repeat = atoi(repeat_str.c_str());
    } else if (i > 0 && argv[i][0] != '-') {
      fnames.push_back(std::string(argv[i]));
    }
  }
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate, NULL, global);
    DCHECK(!context.IsEmpty());
    {
      v8::Context::Scope scope(context);
      double first_parse_total = 0;
      for (size_t i = 0; i < fnames.size(); i++) {
        v8::base::TimeDelta time = RunBaselineParser(
            fnames[i].c_str(), encoding, repeat, isolate, context);
        first_parse_total += time.InMillisecondsF();
      }
      if (benchmark.empty()) benchmark = "Baseline";
      printf("%s(ParseRunTime): %.f ms\n", benchmark.c_str(),
             first_parse_total);
    }
  }
  v8::V8::Dispose();
  v8::V8::ShutdownPlatform();
  delete create_params.array_buffer_allocator;
  return 0;
}
