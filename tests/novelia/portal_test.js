const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1];
const nodes = new Map([...source.matchAll(/\bid="([^"]+)"/g)].map(([, id]) =>
  [id, { value: '', checked: false, textContent: '',
         type: ['proxy_enabled', 'skip_cert_verify'].includes(id) ? 'checkbox' : 'text' }]));
const element = (id) => nodes.get(id) || null;
const requests = [];
let fail = false;
let cleared = 0;
const context = vm.createContext({
  document: { getElementById: element },
  location: { pathname: '/test-session/' },
  setInterval: () => 1,
  clearInterval: () => cleared++,
  fetch: async (_, options) => {
    if (fail) throw Error('temporary network failure');
    if (options) requests.push(JSON.parse(options.body));
    return {
      ok: true,
      text: async () => 'accepted',
      json: async () => ({ message: 'ready', config: { server: 'old.example', port: 443, username: 'reader', proxy_enabled: true, skip_cert_verify: false, has_proxy_password: true } })
    };
  }
});
(async () => {
  vm.runInContext(script, context);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(element('server').value, 'old.example');
  assert.equal(Number(element('port').value), 443);
  assert.equal(element('username').value, 'reader');
  assert.equal(element('proxy_enabled').checked, true);
  assert.equal(element('skip_cert_verify').checked, false);
  assert.equal(element('has_proxy_password'), null);
  assert.match(element('proxy_password').placeholder, /已保存/);
  element('server').value = 'current.example';
  element('port').value = '20004';
  element('sni').value = 'sni.example';
  element('proxy_password').value = 'typed-password';
  element('proxy_enabled').checked = true;
  element('skip_cert_verify').checked = true;
  await element('test').onclick();
  assert.deepEqual(requests[0], {
    action: 'test', server: 'current.example', port: 20004, sni: 'sni.example',
    proxy_password: 'typed-password', proxy_enabled: true, skip_cert_verify: true
  });
  fail = true;
  await context.poll();
  assert.match(element('status').textContent, /重试/);
  assert.equal(cleared, 0);
  console.log('PASS: test submits current proxy form; polling survives temporary disconnects');
})().catch((error) => { console.error(error); process.exitCode = 1; });
