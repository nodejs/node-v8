// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --wasm-deopt --allow-natives-syntax --turboshaft-wasm
// Flags: --experimental-wasm-inlining --liftoff
// Flags: --turboshaft-wasm-instruction-selection-staged
// Flags: --wasm-inlining-ignore-call-counts
// Flags: --wasm-inlining-factor=10

d8.file.execute("test/mjsunit/wasm/wasm-module-builder.js");

// Test deopt with many params with different types, some tagged, some untagged.
(function TestManyParamsTagged() {
  let create = (function() {
    let builder = new WasmModuleBuilder();
    let struct = builder.addStruct([makeField(kWasmI32, false)]);
    let array = builder.addArray(kWasmI32, true);
    builder.addFunction("struct", makeSig([kWasmI32], [wasmRefType(struct)]))
      .addBody([
        kExprLocalGet, 0,
        kGCPrefix, kExprStructNew, struct,
      ]).exportFunc();
    builder.addFunction("array", makeSig([kWasmI32], [wasmRefType(array)]))
      .addBody([
        kExprLocalGet, 0,
        kGCPrefix, kExprArrayNewFixed, array, 1,
      ]).exportFunc();
    return builder.instantiate().exports;
  })();

  let paramCount = 30;
  let builder = new WasmModuleBuilder();
  let struct = builder.addStruct([makeField(kWasmI32, false)]);
  let array = builder.addArray(kWasmI32, true);

  let types = [
    {type: kWasmI32, toI32: [], fromI32: (v) => v},
    {type: wasmRefType(struct), toI32: [kGCPrefix, kExprStructGet, struct, 0],
     fromI32: (v) => create.struct(v)},
    {type: kWasmF64, toI32: [kExprI32SConvertF64], fromI32: (v) => v},
    {type: wasmRefType(array),
     toI32: [kExprI32Const, 0, kGCPrefix, kExprArrayGet, array],
     fromI32: (v) => create.array(v)},
    {type: kWasmF32, toI32: [kExprI32SConvertF32], fromI32: (v) => v},
    {type: kWasmI31Ref, toI32: [kGCPrefix, kExprI31GetS], fromI32: (v) => v},
    {type: kWasmI64, toI32: [kExprI32ConvertI64], fromI32: (v) => BigInt(v)},
  ];

  let calleeParams = new Array(paramCount).fill()
    .map((_, i) => types[i % types.length].type);
  let funcRefT = builder.addType(makeSig(calleeParams, [kWasmI32]));

  builder.addFunction("add", funcRefT)
    .addBody(generateCalleeBody(kExprI32Add)).exportFunc();
  builder.addFunction("sub", funcRefT)
    .addBody(generateCalleeBody(kExprI32Sub)).exportFunc();

  let mainParams = [...calleeParams, wasmRefType(funcRefT)];
  builder.addFunction("main", makeSig(mainParams, [kWasmI32]))
    .addBody([
      ...pushArgs(paramCount + 1),
      kExprCallRef, funcRefT,
  ]).exportFunc();

  // [0, 1, ..., paramCount - 1]
  let values = [...Array(paramCount).keys()];
  let valuesTyped = values.map((_, i) => types[i % types.length].fromI32(i));
  let expectedSum = values.reduce((a, b) => a + b);
  let expectedDiff = values.reduce((a, b) => a - b);
  assertEquals(expectedSum, -expectedDiff);

  let wasm = builder.instantiate().exports;
  assertEquals(expectedSum, wasm.main(...valuesTyped, wasm.add));
  %WasmTierUpFunction(wasm.main);
  assertEquals(expectedSum, wasm.main(...valuesTyped, wasm.add));
  assertTrue(%IsTurboFanFunction(wasm.main));
  assertEquals(expectedDiff, wasm.main(...valuesTyped, wasm.sub));
  assertFalse(%IsTurboFanFunction(wasm.main));


  function generateCalleeBody(binop) {
    let result = [kExprLocalGet, 0, ...types[0].toI32];
    for (let i = 1; i < paramCount; ++i) {
      result.push(kExprLocalGet, i, ...types[i % types.length].toI32, binop);
    }
    return result;
  }

  function pushArgs(paramCount) {
    let result = [];
    for (let i = 0; i < paramCount; ++i) {
      result.push(kExprLocalGet, i);
    }
    return result;
  }
})();
