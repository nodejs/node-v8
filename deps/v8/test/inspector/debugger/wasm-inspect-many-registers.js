// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const {session, contextGroup, Protocol} =
    InspectorTest.start('Test inspecting register values in Liftoff.');

utils.load('test/mjsunit/wasm/wasm-module-builder.js');

const num_locals = 10;
const configs = {
  i32: {type: kWasmI32, add: kExprI32Add, from_i32: kExprNop},
  i64: {type: kWasmI64, add: kExprI64Add, from_i32: kExprI64SConvertI32},
  f32: {type: kWasmF32, add: kExprF32Add, from_i32: kExprF32SConvertI32},
  f64: {type: kWasmF64, add: kExprF64Add, from_i32: kExprF64SConvertI32}
};

function instantiate(bytes) {
  let buffer = new ArrayBuffer(bytes.length);
  let view = new Uint8Array(buffer);
  for (let i = 0; i < bytes.length; ++i) {
    view[i] = bytes[i] | 0;
  }

  let module = new WebAssembly.Module(buffer);
  return new WebAssembly.Instance(module);
}

const evalWithUrl = (code, url) => Protocol.Runtime.evaluate(
    {'expression': code + '\n//# sourceURL=v8://test/' + url});

function getWasmValue(value) {
  return typeof (value.value) === 'undefined' ? value.unserializableValue :
                                                value.value;
}

Protocol.Debugger.onPaused(async msg => {
  let loc = msg.params.callFrames[0].location;
  let line = [`Paused at offset ${loc.columnNumber}`];
  // Inspect only the top wasm frame.
  var frame = msg.params.callFrames[0];
  for (var scope of frame.scopeChain) {
    if (scope.type == 'module') continue;
    var scope_properties =
        await Protocol.Runtime.getProperties({objectId: scope.object.objectId});
    if (scope.type == 'local') {
      for (var value of scope_properties.result.result) {
        let msg = await Protocol.Runtime.getProperties(
          {objectId: value.value.objectId});
        let str = msg.result.result.map(elem => getWasmValue(elem.value)).join(', ');
        line.push(`${value.name}: [${str}]`);
      }
    } else {
      let str = scope_properties.result.result.map(elem => getWasmValue(elem.value)).join(', ');
      line.push(`${scope.type}: [${str}]`);
    }
  }
  InspectorTest.log(line.join('; '));
  Protocol.Debugger.resume();
});

// Build a function which receives a lot of arguments. It loads them all and
// adds them together.
// In Liftoff, this will hold many values in registers at the break sites.
function buildModuleBytes(config) {
  const sig = makeSig(
      new Array(num_locals).fill(configs[config].type), [configs[config].type]);
  const body = [];
  for (let i = 0; i < num_locals; ++i) body.push(kExprLocalGet, i);
  for (let i = 0; i < num_locals - 1; ++i) body.push(configs[config].add);
  body.push(kExprReturn);
  const builder = new WasmModuleBuilder();
  const test_func = builder.addFunction('test_' + config, sig).addBody(body);
  const main_body = [];
  for (let i = 0; i < num_locals; ++i)
    main_body.push(kExprI32Const, i, configs[config].from_i32);
  main_body.push(kExprCallFunction, test_func.index, kExprDrop);
  const main =
      builder.addFunction('main', kSig_v_v).addBody(main_body).exportAs('main');

  const module_bytes = builder.toArray();

  // Break at every {kExprLocalGet} and at every addition.
  const interesting_opcodes = [kExprLocalGet, kExprReturn, configs[config].add];
  const breakpoints = [];
  for (let idx = 0; idx < body.length; ++idx) {
    if (interesting_opcodes.find(elem => elem == body[idx])) {
      breakpoints.push(test_func.body_offset + idx);
    }
  }

  return [module_bytes, breakpoints];
}

async function testConfig(config) {
  InspectorTest.log(`Testing ${config}.`);
  const [module_bytes, breakpoints] = buildModuleBytes(config);
  const instance_name = `instance_${config}`;
  // Spawn asynchronously:
  let instantiate_code = evalWithUrl(
      `const ${instance_name} = instantiate(${JSON.stringify(module_bytes)});`,
      'instantiate');
  InspectorTest.log('Waiting for wasm script.');
  const [, {params: wasm_script}] = await Protocol.Debugger.onceScriptParsed(2);
  InspectorTest.log(`Setting ${breakpoints.length} breakpoints.`);
  for (let offset of breakpoints) {
    await Protocol.Debugger.setBreakpoint({
      'location': {
        'scriptId': wasm_script.scriptId,
        'lineNumber': 0,
        'columnNumber': offset
      }
    });
  }
  InspectorTest.log('Calling main.');
  await evalWithUrl(`${instance_name}.exports.main()`, `run_${config}`);
  InspectorTest.log('main returned.');
}

(async function test() {
  await Protocol.Debugger.enable();
  InspectorTest.log('Installing instantiate function.');
  await evalWithUrl(instantiate, 'install_instantiate');
  for (let config in configs) {
    await testConfig(config);
  }
  InspectorTest.log('Finished!');
  InspectorTest.completeTest();
})();
