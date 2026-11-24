const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1];
const nodes = new Map([...source.matchAll(/\bid="([^"]+)"/g)].map(([, id]) =>
  [id, { value: '', checked: false, textContent: '', hidden: false,
         type: ['proxy_enabled', 'skip_cert_verify'].includes(id) ? 'checkbox' : 'text' }]));
const element = (id) => nodes.get(id) || null;
const requests = [];
const scripts = [];
let captcha = "";
let fail = false;
let siteConfig = { app_name: 'Novelia' };
let updatedProxy = {};
let cleared = 0;
const context = vm.createContext({
  document: { getElementById: element, createElement: () => ({}),
    head: { appendChild: script => scripts.push(script) } },
  window: {}, hcaptcha: { render: () => 0, getResponse: () => captcha, reset: () => { captcha = ''; } },
  location: { origin: 'http://192.168.1.9:8081', pathname: '/test-session/', hash: '' },
  URL, URLSearchParams,
  history: { replaceState() {} },
  setInterval: () => 1,
  clearInterval: () => cleared++,
  fetch: async (_, options) => {
    if (fail) throw Error('temporary network failure');
    if (options) requests.push(JSON.parse(options.body));
    return {
      ok: true,
      text: async () => 'accepted',
      json: async () => ({
        message: 'ready',
        config: {
          ...siteConfig,
          server: 'old.example', port: 443, username: 'reader',
          proxy_enabled: true, skip_cert_verify: false, has_proxy_password: true,
          ...updatedProxy
        }
      })
    };
  }
});
(async () => {
  vm.runInContext(script, context);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(element('app').textContent, 'Novelia');
  assert.equal(element('server').value, 'old.example');
  assert.equal(Number(element('port').value), 443);
  assert.equal(element('username').value, 'reader');
  assert.equal(element('proxy_enabled').checked, true);
  assert.equal(element('skip_cert_verify').checked, false);
  assert.equal(element('has_proxy_password'), null);
  assert.equal(element('useNoveliaProxy'), null);
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
  fail = false;
  siteConfig = {
    app_name: 'Novalpie',
    hcaptcha_sitekey: 'sitekey-fixture',
    allow_session_import: true,
    session_import_hint: '复制 auth_token'
  };
  vm.runInContext('initialized = false', context);
  await context.poll();
  assert.equal(element('app').textContent, 'Novalpie');
  assert.equal(element('login').hidden, false);
  assert.equal(element('sessionImport').hidden, false);
  assert.equal(element('sessionHint').textContent, '复制 auth_token');
  assert.match(scripts[0].src, /^https:\/\/js\.hcaptcha\.com\//);
  context.window.renderPortalCaptcha();
  element('username').value = ' reader ';
  element('password').value = 'fixture-password';
  await element('login').onsubmit({ preventDefault() {} });
  assert.equal(requests.length, 1);
  assert.match(element('captchaStatus').textContent, /人机验证/);
  captcha = 'fixture-challenge';
  await element('login').onsubmit({ preventDefault() {} });
  assert.deepEqual(requests[1], {
    action: 'login', username: 'reader', password: 'fixture-password', captcha: 'fixture-challenge'
  });
  assert.equal(element('password').value, '');
  assert.equal(captcha, '');
  element('token').value = ' reader-token ';
  element('session').onsubmit({ preventDefault() {} });
  assert.deepEqual(requests[2], { action: 'login', token: 'reader-token' });
  assert.equal(element('token').value, '');
  element('username').value = 'typed-reader';
  updatedProxy = { server: 'shared.example', port: 20004, sni: 'sni.example', skip_cert_verify: true };
  await context.poll();
  assert.equal(element('server').value, 'shared.example');
  assert.equal(element('sni').value, 'sni.example');
  assert.equal(element('skip_cert_verify').checked, true);
  assert.equal(element('username').value, 'typed-reader');
  console.log('PASS: proxy form, reconnect, generic capabilities, captcha and session import');
})().catch((error) => { console.error(error); process.exitCode = 1; });
