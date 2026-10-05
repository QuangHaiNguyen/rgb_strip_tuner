#!/usr/bin/env node
/**
 * @file test_updater_page_js.js
 * @brief SPEC-007 T-19 / FR-25 host part: runs the script of the updater page (updater/main/updater_page.c, printed by
 *        dump_updater_page) in Node against a minimal DOM and a scripted XMLHttpRequest, and exercises the Upload
 *        button: the no-file and .bin checks, the GET /status pre-check (uploading/done -> "Upload in progress",
 *        request error or non-200 -> "Send failed, check connection", nothing sent), the raw octet-stream POST,
 *        progress and result texts, the button re-enabled on every outcome, and no timer or automatic retry.
 *        Same approach as test/tuner-read-measurement/page_js/test_read_page_js.js.
 *
 * Usage: node test_updater_page_js.js <dump_updater_page executable>
 * Exit code 0 when every case passes, 1 otherwise.
 */
'use strict';

const { execFileSync } = require('child_process');

const [dumpExecutable] = process.argv.slice(2);
if (!dumpExecutable) {
    console.error('usage: test_updater_page_js.js <dump_updater_page>');
    process.exit(2);
}
const html = execFileSync(dumpExecutable, [], { encoding: 'utf8' });
const scriptMatch = /<script>([\s\S]*)<\/script>/.exec(html);
if (!scriptMatch) {
    console.error('no <script> found in the page');
    process.exit(1);
}
const pageScript = scriptMatch[1];

// ---- Minimal world: elements by id, scripted XHR, timers that fail the test -----------------------------------------

function createWorld() {
    const world = { failures: [], xhrs: [], timers: 0 };
    world.fail = (message) => world.failures.push(message);
    const status = { id: 's', textWrites: [], _text: '' };
    Object.defineProperty(status, 'textContent', {
        get() { return this._text; },
        set(text) { this._text = String(text); this.textWrites.push(this._text); },
    });
    Object.defineProperty(status, 'innerHTML', {
        get() { world.fail('innerHTML read'); return ''; },
        set() { world.fail('innerHTML written (fixed texts must use textContent)'); },
    });
    const button = { id: 'u', disabled: false, onclick: null };
    const input = { id: 'f', files: [] };
    world.elements = { s: status, u: button, f: input };
    world.document = {
        getElementById(id) {
            if (!world.elements[id]) world.fail(`getElementById('${id}') found nothing`);
            return world.elements[id] || null;
        },
    };
    world.XMLHttpRequest = function XMLHttpRequest() {
        const xhr = {
            method: null, url: null, headers: {}, sent: false, body: undefined, status: 0, responseText: '',
            onload: null, onerror: null, upload: { onprogress: null },
            disabledAtSend: undefined,
            open(method, url) { this.method = method; this.url = url; },
            setRequestHeader(name, value) { this.headers[name.toLowerCase()] = value; },
            send(body) {
                if (this.sent) world.fail(`${this.method} ${this.url} sent twice`);
                this.sent = true;
                this.body = body;
                this.disabledAtSend = button.disabled;
            },
        };
        world.xhrs.push(xhr);
        return xhr;
    };
    world.setTimeout = () => { world.timers++; world.fail('setTimeout used (no timer or retry, FR-25)'); };
    world.setInterval = () => { world.timers++; world.fail('setInterval used (no timer or retry, FR-25)'); };
    world.fetch = () => { world.fail('fetch used: the page uses XMLHttpRequest'); return new Promise(() => {}); };
    const run = new Function('document', 'XMLHttpRequest', 'setTimeout', 'setInterval', 'fetch', pageScript);
    run(world.document, world.XMLHttpRequest, world.setTimeout, world.setInterval, world.fetch);
    world.click = () => {
        if (typeof button.onclick !== 'function') { world.fail('Upload button has no onclick'); return; }
        button.onclick();
    };
    world.selectFile = (name) => { input.files = name === null ? [] : [{ name, size: 1000 }]; };
    world.sentXhrs = () => world.xhrs.filter((xhr) => xhr.sent);
    world.posts = () => world.sentXhrs().filter((xhr) => xhr.method === 'POST');
    world.text = () => status.textContent;
    return world;
}

function respond(xhr, status, text) {
    xhr.status = status;
    xhr.responseText = text;
    if (typeof xhr.onload !== 'function') throw new Error(`${xhr.method} ${xhr.url} has no onload`);
    xhr.onload();
}

function networkError(xhr) {
    xhr.status = 0;
    if (typeof xhr.onerror !== 'function') throw new Error(`${xhr.method} ${xhr.url} has no onerror`);
    xhr.onerror();
}

function check(world, condition, message) { if (!condition) world.fail(message); }

/** Click with a .bin file and return the GET /status request, checking the pre-check is sent first. */
function startUpload(world, name = 'rgb_strip_tuner.bin') {
    world.selectFile(name);
    world.click();
    const sent = world.sentXhrs();
    check(world, sent.length === 1, `expected 1 request after the click, got ${sent.length}`);
    const statusXhr = sent[0];
    check(world, statusXhr && statusXhr.method === 'GET' && statusXhr.url === '/status',
          `first request must be GET /status, got ${statusXhr && statusXhr.method} ${statusXhr && statusXhr.url}`);
    check(world, statusXhr && statusXhr.body === undefined, 'GET /status must not carry the file');
    check(world, world.elements.u.disabled === true, 'button must be disabled during the /status check');
    check(world, world.posts().length === 0, 'no POST before the /status answer');
    return statusXhr;
}

function expectNoPost(world, expectedText) {
    check(world, world.posts().length === 0, `no POST expected, got ${world.posts().length}`);
    check(world, world.text() === expectedText, `status text "${world.text()}", expected "${expectedText}"`);
    check(world, world.elements.u.disabled === false, 'button must be re-enabled');
}

// ---- Cases ---------------------------------------------------------------------------------------------------------

const cases = {
    'no file: "Select a .bin file", nothing sent'(world) {
        world.selectFile(null);
        world.click();
        check(world, world.xhrs.length === 0, 'no request expected');
        check(world, world.text() === 'Select a .bin file', `text "${world.text()}"`);
        check(world, world.elements.u.disabled === false, 'button stays enabled');
    },
    'not a .bin name: "Not a .bin file", nothing sent'(world) {
        for (const name of ['firmware.txt', 'firmware.bin.zip', 'bin', 'firmware.bi']) {
            world.selectFile(name);
            world.click();
            check(world, world.xhrs.length === 0, `no request expected for ${name}`);
            check(world, world.text() === 'Not a .bin file', `text "${world.text()}" for ${name}`);
        }
        check(world, world.elements.u.disabled === false, 'button stays enabled');
    },
    '.BIN in upper case is accepted (any case)'(world) {
        const statusXhr = startUpload(world, 'FIRMWARE.BIN');
        respond(statusXhr, 200, 'state=idle&received=0&total=0&installed=none');
        check(world, world.posts().length === 1, 'POST expected');
    },
    'state=idle -> raw octet-stream POST /update with the file, progress, result text, button re-enabled'(world) {
        const statusXhr = startUpload(world);
        respond(statusXhr, 200, 'state=idle&received=0&total=0&installed=01.00.00');
        const posts = world.posts();
        check(world, posts.length === 1, `1 POST expected, got ${posts.length}`);
        const post = posts[0];
        check(world, post.url === '/update', `POST url ${post.url}`);
        check(world, post.headers['content-type'] === 'application/octet-stream', 'Content-Type octet-stream');
        check(world, post.body === world.elements.f.files[0], 'the raw File object is the body (no FormData)');
        check(world, post.disabledAtSend === true, 'button disabled while uploading');
        check(world, world.text() === 'Uploading... 0 %', `text "${world.text()}"`);
        post.upload.onprogress({ lengthComputable: true, loaded: 500, total: 1000 });
        check(world, world.text() === 'Uploading... 50 %', `progress text "${world.text()}"`);
        respond(post, 200, 'Update complete, restarting');
        check(world, world.text() === 'Update complete, restarting', `result text "${world.text()}"`);
        check(world, world.elements.u.disabled === false, 'button re-enabled after the result');
        check(world, world.sentXhrs().length === 2, 'exactly GET /status + POST /update');
    },
    'state=error -> the upload proceeds'(world) {
        const statusXhr = startUpload(world);
        respond(statusXhr, 200, 'state=error&received=0&total=10000&installed=none');
        check(world, world.posts().length === 1, 'POST expected after an earlier error');
    },
    'state=uploading -> "Upload in progress", nothing sent'(world) {
        respond(startUpload(world), 200, 'state=uploading&received=4096&total=10000&installed=none');
        expectNoPost(world, 'Upload in progress');
    },
    'state=done -> "Upload in progress", nothing sent'(world) {
        respond(startUpload(world), 200, 'state=done&received=10000&total=10000&installed=01.02.03');
        expectNoPost(world, 'Upload in progress');
    },
    '/status network error -> "Send failed, check connection", nothing sent'(world) {
        networkError(startUpload(world));
        expectNoPost(world, 'Send failed, check connection');
    },
    '/status non-200 (404, 500, 503) -> "Send failed, check connection", nothing sent'(world) {
        for (const code of [404, 500, 503]) {
            const before = world.posts().length;
            respond(startUpload2(world), code, 'state=idle&received=0&total=0&installed=none');
            check(world, world.posts().length === before, `no POST after /status ${code}`);
            check(world, world.text() === 'Send failed, check connection', `text "${world.text()}" for ${code}`);
            check(world, world.elements.u.disabled === false, `button re-enabled after ${code}`);
        }
    },
    'POST network error -> "Send failed", button re-enabled, no retry'(world) {
        respond(startUpload(world), 200, 'state=idle&received=0&total=0&installed=none');
        networkError(world.posts()[0]);
        check(world, world.text() === 'Send failed', `text "${world.text()}"`);
        check(world, world.elements.u.disabled === false, 'button re-enabled');
        check(world, world.sentXhrs().length === 2, 'no automatic retry');
    },
    'POST rejected by the server (409 / 400) -> the server text is shown, no retry'(world) {
        respond(startUpload(world), 200, 'state=idle&received=0&total=0&installed=none');
        respond(world.posts()[0], 409, 'Upload in progress');
        check(world, world.text() === 'Upload in progress', `text "${world.text()}"`);
        check(world, world.elements.u.disabled === false, 'button re-enabled');
        check(world, world.sentXhrs().length === 2, 'no automatic retry');
    },
    'a new click after "Upload in progress" checks /status again (user-driven, not automatic)'(world) {
        respond(startUpload(world), 200, 'state=uploading&received=1&total=2&installed=none');
        world.click();
        const sent = world.sentXhrs();
        check(world, sent.length === 2 && sent[1].method === 'GET' && sent[1].url === '/status', 'second /status');
        respond(sent[1], 200, 'state=idle&received=0&total=0&installed=none');
        check(world, world.posts().length === 1, 'POST after the state became idle');
    },
};

/** Like startUpload() but tolerant of earlier requests in the same world (for the loop case). */
function startUpload2(world) {
    world.selectFile('a.bin');
    const before = world.sentXhrs().length;
    world.click();
    const sent = world.sentXhrs();
    check(world, sent.length === before + 1, 'one request per click');
    const xhr = sent[sent.length - 1];
    check(world, xhr.method === 'GET' && xhr.url === '/status', 'GET /status first');
    return xhr;
}

let failed = 0;
for (const [name, body] of Object.entries(cases)) {
    const world = createWorld();
    try {
        body(world);
    } catch (error) {
        world.fail(`exception: ${error.message}`);
    }
    if (world.timers !== 0) world.fail('a timer was created');
    if (world.failures.length === 0) {
        console.log(`PASS ${name}`);
    } else {
        failed++;
        console.log(`FAIL ${name}`);
        for (const failure of world.failures) console.log(`     ${failure}`);
    }
}
console.log(`${Object.keys(cases).length - failed} of ${Object.keys(cases).length} page-script cases passed`);
process.exit(failed === 0 ? 0 : 1);
