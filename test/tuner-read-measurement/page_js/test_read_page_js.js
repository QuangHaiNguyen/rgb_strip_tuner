#!/usr/bin/env node
/**
 * @file test_read_page_js.js
 * @brief SPEC-006 T-6 host part (FR-1 to FR-6, FR-20): runs the page script embedded by main/http_portal/tuner_page.c
 *        in Node against a minimal DOM, a scripted fetch() and a manual setTimeout() queue, and exercises the Read
 *        button. Same harness approach as test/ws2812-tuner-page/page_js/test_tuner_page_js.js (SPEC-003 T-21).
 *
 * Usage: node test_read_page_js.js <dump_tuner_page executable> provisioning|station
 * Exit code 0 when every case passes, 1 otherwise.
 */
'use strict';

const { execFileSync } = require('child_process');

const [dumpExecutable, pageName] = process.argv.slice(2);
if (!dumpExecutable || !['provisioning', 'station'].includes(pageName)) {
    console.error('usage: test_read_page_js.js <dump_tuner_page> provisioning|station');
    process.exit(2);
}
const html = execFileSync(dumpExecutable, [pageName], { encoding: 'utf8' });
const scriptMatch = /<script>([\s\S]*)<\/script>/.exec(html);
if (!scriptMatch) {
    console.error('no <script> found in the page');
    process.exit(1);
}
const pageScript = scriptMatch[1];

// ---- Minimal DOM built from the page's own markup -------------------------------------------------------------------

function parseElements(markup) {
    const elements = {};
    const tagPattern = /<(\w+)([^>]*)>/g;
    let match;
    while ((match = tagPattern.exec(markup)) !== null) {
        const attributes = {};
        const attributePattern = /(\w[\w-]*)(?:=("[^"]*"|'[^']*'|[^\s>]+))?/g;
        let attribute;
        while ((attribute = attributePattern.exec(match[2])) !== null) {
            attributes[attribute[1]] = attribute[2] === undefined ? '' : attribute[2].replace(/^["']|["']$/g, '');
        }
        if (attributes.id) elements[attributes.id] = { tag: match[1], attributes };
    }
    return elements;
}

class FakeElement {
    constructor(id, tag, attributes, world) {
        this.id = id;
        this.tag = tag;
        this.world = world;
        this.style = {};
        this.listeners = {};
        this._text = '';
        this.textWrites = [];
        this.min = attributes.min === undefined ? undefined : Number(attributes.min);
        this.max = attributes.max === undefined ? undefined : Number(attributes.max);
        this.step = attributes.step === undefined ? undefined : Number(attributes.step);
        this.defaultValue = attributes.value === undefined ? '' : attributes.value;
        this.value = this.defaultValue;
        this.customValidity = '';
        this.validityWrites = 0;
        this.oninput = null;
        this.onclick = null;
    }
    get textContent() { return this._text; }
    set textContent(text) { this._text = String(text); this.textWrites.push(this._text); }
    get innerHTML() { this.world.fail(`innerHTML read on #${this.id}`); return ''; }
    set innerHTML(_) { this.world.fail(`innerHTML written on #${this.id} (FR-4: textContent only)`); }
    set outerHTML(_) { this.world.fail(`outerHTML written on #${this.id}`); }
    insertAdjacentHTML() { this.world.fail(`insertAdjacentHTML on #${this.id}`); }
    setCustomValidity(text) { this.customValidity = String(text); this.validityWrites++; }
    checkValidity() {
        if (this.customValidity !== '') return false;
        if (this.min === undefined) return true;
        const value = Number(this.value);
        if (this.value === '' || Number.isNaN(value)) return false;
        if (value < this.min || value > this.max) return false;
        return (value - this.min) % this.step === 0;
    }
    addEventListener(type, listener) { (this.listeners[type] = this.listeners[type] || []).push(listener); }
}

function createWorld() {
    const world = {
        failures: [],
        fail(message) { this.failures.push(message); },
        now: 0,
        timers: [],
        timerCount: 0,
        fetches: [],
        intervals: 0,
    };
    const definitions = parseElements(html);
    world.elements = {};
    for (const [id, definition] of Object.entries(definitions)) {
        world.elements[id] = new FakeElement(id, definition.tag, definition.attributes, world);
    }
    world.document = {
        getElementById(id) {
            const element = world.elements[id];
            if (!element) world.fail(`getElementById('${id}') found nothing`);
            return element || null;
        },
    };
    world.setTimeout = (callback, delay) => {
        world.timerCount++;
        world.timers.push({ callback, delay, createdAt: world.now, outstandingFetches: world.pendingFetches().length });
        return world.timerCount;
    };
    world.setInterval = () => { world.intervals++; world.fail('setInterval used (FR-5)'); };
    world.fetch = (url, options) => {
        const entry = { url, options: options || {}, at: world.now, settled: false };
        entry.promise = new Promise((resolve, reject) => { entry.resolve = resolve; entry.reject = reject; });
        world.fetches.push(entry);
        return entry.promise;
    };
    world.pendingFetches = () => world.fetches.filter((entry) => !entry.settled);
    return world;
}

function response(status, body, headers) {
    const lower = {};
    for (const [name, value] of Object.entries(headers || {})) lower[name.toLowerCase()] = value;
    return {
        ok: status >= 200 && status < 300,
        status,
        headers: { get: (name) => (lower[name.toLowerCase()] === undefined ? null : lower[name.toLowerCase()]) },
        text: () => Promise.resolve(body),
    };
}

async function flush() {
    for (let round = 0; round < 10; round++) await new Promise((resolve) => setImmediate(resolve));
}

function loadPage() {
    const world = createWorld();
    const run = new Function('document', 'fetch', 'setTimeout', 'setInterval', 'URLSearchParams', pageScript);
    run(world.document, world.fetch, world.setTimeout, world.setInterval, URLSearchParams);
    return world;
}

// ---- Helpers acting like a user and like the device ---------------------------------------------------------------

const el = (world, id) => world.elements[id];
const status = (world) => el(world, 'st').textContent;
const INPUTS = ['b0h', 'b0p', 'b1h', 'b1p', 'rst'];
const READOUTS = ['b0v', 'b0d', 'b1v', 'b1d'];

function setInput(world, id, value) {
    const input = el(world, id);
    input.value = String(value);
    if (input.oninput) input.oninput({ target: input });
    for (const listener of el(world, 'f').listeners.input || []) listener({ target: input });
}

function pressDefaults(world) {
    for (const listener of el(world, 'f').listeners.reset || []) listener({});
}

function clickSend(world) { el(world, 'sd').onclick({}); }
function clickRead(world) {
    const button = el(world, 'rd');
    if (!button || typeof button.onclick !== 'function') throw new Error('no Read button handler (#rd)');
    button.onclick({});
}

async function answer(world, entry, statusCode, body, headers) {
    entry.settled = true;
    entry.resolve(response(statusCode, body, headers));
    await flush();
}

async function failNetwork(world, entry) {
    entry.settled = true;
    entry.reject(new TypeError('Failed to fetch'));
    await flush();
}

async function runTimer(world) {
    const timer = world.timers.shift();
    if (!timer) throw new Error('no timer pending');
    world.now += timer.delay;
    timer.callback();
    await flush();
    return timer;
}

const posts = (world) => world.fetches.filter((entry) => entry.options.method === 'POST');
const readPosts = (world) => posts(world).filter((entry) => entry.url === '/tuner/read');
const polls = (world) => world.fetches.filter((entry) => entry.url.startsWith('/tuner/result'));
const lastFetch = (world) => world.fetches[world.fetches.length - 1];

async function readAndAccept(world, seq) {
    clickRead(world);
    const post = lastFetch(world);
    await answer(world, post, 200, 'Reading', seq === null ? {} : { 'Tuner-Seq': String(seq) });
    return post;
}

async function sendAndAccept(world, seq) {
    clickSend(world);
    await answer(world, lastFetch(world), 200, 'Sent', { 'Tuner-Seq': String(seq) });
}

async function pollOnce(world, body, statusCode = 200) {
    const before = world.fetches.length;
    await runTimer(world);
    if (world.fetches.length !== before + 1) throw new Error('the timer issued no GET /tuner/result');
    await answer(world, lastFetch(world), statusCode, body, {});
    return lastFetch(world);
}

function snapshot(world) {
    const state = {};
    for (const id of INPUTS) state[id] = [el(world, id).value, el(world, id).customValidity];
    for (const id of READOUTS) state[id] = el(world, id).textContent;
    for (const id of ['b0w', 'b1w']) state[id] = el(world, id).style.width;
    return JSON.stringify(state);
}

// ---- Cases -----------------------------------------------------------------------------------------------------------

const cases = [];
const test = (name, ids, body) => cases.push({ name, ids, body });
function expect(condition, message) { if (!condition) throw new Error(message); }
function equal(actual, expected, what) {
    if (actual !== expected) throw new Error(`${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

const kReadDone = 'state=read&b0h=400&b0p=1250&b1h=800&b1p=1250';
const kReadSummary = 'Read\nBit 0: high 400 ns, period 1250 ns\nBit 1: high 800 ns, period 1250 ns';

test('the Read button exists, is a type=button between Send and Defaults', 'FR-1', async () => {
    const world = loadPage();
    const rd = el(world, 'rd');
    expect(rd !== undefined, 'no #rd element');
    equal(rd.tag, 'button', '#rd tag');
    const order = [...html.matchAll(/<button[^>]*>([^<]*)<\/button>/g)].map((m) => m[1]);
    equal(JSON.stringify(order), JSON.stringify(['Send', 'Read', 'Defaults']), 'button order');
    return world;
});

test('Read press: Reading..., exactly one POST /tuner/read with no body, no timer while it is in flight', 'FR-2 FR-5', async () => {
    const world = loadPage();
    clickRead(world);
    equal(status(world), 'Reading...', 'status');
    equal(world.fetches.length, 1, 'requests');
    const post = world.fetches[0];
    equal(post.url, '/tuner/read', 'URL');
    equal(post.options.method, 'POST', 'method');
    equal(post.options.body, undefined, 'body');
    equal(world.timerCount, 0, 'timers');
    return world;
});

test('200 with Tuner-Seq: first poll 250 ms after the response, GET /tuner/result?seq=<n> no-store', 'FR-3 FR-5', async () => {
    const world = loadPage();
    await readAndAccept(world, 7);
    equal(world.timers.length, 1, 'one timer');
    equal(world.timers[0].delay, 250, 'poll delay');
    equal(world.timers[0].outstandingFetches, 0, 'timer created after the response');
    await runTimer(world);
    equal(polls(world).length, 1, 'GETs');
    equal(polls(world)[0].url, '/tuner/result?seq=7', 'poll URL');
    equal(polls(world)[0].options.cache, 'no-store', 'cache mode');
    equal(polls(world)[0].options.method, undefined, 'GET');
    return world;
});

test('state=read: Read plus the two FR-4 lines, polling stops', 'FR-4 FR-20', async () => {
    const world = loadPage();
    await readAndAccept(world, 3);
    await pollOnce(world, kReadDone);
    equal(status(world), kReadSummary, 'status');
    equal(world.timers.length, 0, 'no further timer');
    expect(!status(world).toLowerCase().includes('duty'), 'no duty value');
    return world;
});

test('state=read with bit 0 n/a shows Bit 0: not found', 'FR-4', async () => {
    const world = loadPage();
    await readAndAccept(world, 3);
    await pollOnce(world, 'state=read&b0h=n/a&b0p=n/a&b1h=800&b1p=1250');
    equal(status(world), 'Read\nBit 0: not found\nBit 1: high 800 ns, period 1250 ns', 'status');
    return world;
});

test('state=read with bit 1 n/a shows Bit 1: not found', 'FR-4', async () => {
    const world = loadPage();
    await readAndAccept(world, 3);
    await pollOnce(world, 'state=read&b0h=388&b0p=1250&b1h=n/a&b1p=n/a');
    equal(status(world), 'Read\nBit 0: high 388 ns, period 1250 ns\nBit 1: not found', 'status');
    return world;
});

for (const [state, text] of [['timeout', 'Measurement failed: no signal'],
                             ['count_error', 'Measurement failed: bad capture'], ['not_measured', 'Not measured'],
                             ['superseded', 'Superseded by a newer send'], ['unknown', 'Measurement not available'],
                             ['done&b0=400&b1=800&match=144', 'Measurement not available'],
                             ['bogus', 'Measurement not available']]) {
    test(`Read poll final state ${state.split('&')[0]} shows Read + its fixed text and stops`, 'FR-4', async () => {
        const world = loadPage();
        await readAndAccept(world, 5);
        await pollOnce(world, `state=${state}`);
        equal(status(world), `Read\n${text}`, 'status');
        equal(world.timers.length, 0, 'no further timer');
        equal(polls(world).length, 1, 'GETs');
        return world;
    });
}

test('8 pending polls, 250 ms apart, then Read / Measurement not available; no retry POST', 'FR-3 FR-4 FR-5', async () => {
    const world = loadPage();
    await readAndAccept(world, 9);
    for (let poll = 1; poll <= 8; poll++) {
        equal(world.timers.length, 1, `one timer before poll ${poll}`);
        equal(world.timers[0].delay, 250, `delay before poll ${poll}`);
        equal(world.timers[0].outstandingFetches, 0, `timer ${poll} after the previous response`);
        await pollOnce(world, 'state=pending');
    }
    equal(polls(world).length, 8, 'GETs');
    equal(world.timers.length, 0, 'no ninth timer');
    equal(status(world), 'Read\nMeasurement not available', 'status');
    equal(readPosts(world).length, 1, 'POSTs');
    return world;
});

test('a non-200 poll stops with Read / Measurement not available', 'FR-4', async () => {
    const world = loadPage();
    await readAndAccept(world, 2);
    await pollOnce(world, 'Invalid request', 400);
    equal(status(world), 'Read\nMeasurement not available', 'status');
    equal(world.timers.length, 0, 'timers');
    return world;
});

test('a network error during the Read poll stops with Read / Measurement not available', 'FR-4', async () => {
    const world = loadPage();
    await readAndAccept(world, 2);
    await runTimer(world);
    await failNetwork(world, lastFetch(world));
    equal(status(world), 'Read\nMeasurement not available', 'status');
    equal(world.timers.length, 0, 'timers');
    equal(readPosts(world).length, 1, 'POSTs');
    return world;
});

test('200 without Tuner-Seq: Read / Measurement not available, no poll', 'FR-3', async () => {
    const world = loadPage();
    await readAndAccept(world, null);
    equal(status(world), 'Read\nMeasurement not available', 'status');
    equal(world.timerCount, 0, 'timers');
    return world;
});

test('non-200 POST: the body text (Forbidden origin) is shown, no poll', 'FR-3', async () => {
    const world = loadPage();
    clickRead(world);
    await answer(world, lastFetch(world), 403, 'Forbidden origin', { 'Tuner-Seq': '4' });
    equal(status(world), 'Forbidden origin', 'status');
    equal(world.timerCount, 0, 'timers');
    return world;
});

test('network failure on the POST: Read failed, check connection; no retry', 'FR-3', async () => {
    const world = loadPage();
    clickRead(world);
    await failNetwork(world, lastFetch(world));
    equal(status(world), 'Read failed, check connection', 'status');
    equal(world.timerCount, 0, 'timers');
    equal(world.fetches.length, 1, 'requests');
    return world;
});

test('each Read press makes exactly one POST', 'FR-2 FR-5', async () => {
    const world = loadPage();
    for (let press = 1; press <= 3; press++) {
        clickRead(world);
        equal(readPosts(world).length, press, `POSTs after press ${press}`);
    }
    return world;
});

for (const [label, values] of [['out-of-range values', [50, 5000, 1300, 9, 1]], ['empty values', ['', '', '', '', '']],
                               ['a V4 violation', [800, 1250, 400, 1250, 280]], ['a V3 violation', [750, 800, 100, 800, 280]]]) {
    test(`Read works with ${label} and marks no input`, 'FR-1', async () => {
        const world = loadPage();
        INPUTS.forEach((id, index) => setInput(world, id, values[index]));
        const validityWrites = INPUTS.map((id) => el(world, id).validityWrites);
        const before = snapshot(world);
        clickRead(world);
        equal(status(world), 'Reading...', 'status (no validation text)');
        equal(readPosts(world).length, 1, 'POSTs');
        equal(JSON.stringify(INPUTS.map((id) => el(world, id).validityWrites)), JSON.stringify(validityWrites),
              'setCustomValidity calls by Read');
        equal(snapshot(world), before, 'inputs and read-outs');
        return world;
    });
}

test('a read result changes no input, slider, duty display or drawing', 'FR-6', async () => {
    const world = loadPage();
    setInput(world, 'b0h', 425);
    const before = snapshot(world);
    await readAndAccept(world, 1);
    await pollOnce(world, 'state=read&b0h=300&b0p=1100&b1h=900&b1p=1400');
    equal(status(world), 'Read\nBit 0: high 300 ns, period 1100 ns\nBit 1: high 900 ns, period 1400 ns', 'status');
    equal(snapshot(world), before, 'inputs and read-outs');
    return world;
});

for (const [label, action] of [['an input edit', (world) => setInput(world, 'b1h', 825)],
                               ['Defaults', (world) => pressDefaults(world)]]) {
    test(`${label} stops a Read poll and clears the status`, 'FR-2', async () => {
        const world = loadPage();
        await readAndAccept(world, 4);
        action(world);
        equal(status(world), '', 'status cleared');
        while (world.timers.length > 0) await runTimer(world);
        equal(polls(world).length, 0, 'no GET after the stop');
        return world;
    });
    test(`${label} while a Read poll is in flight: its late response is ignored`, 'FR-2 FR-3', async () => {
        const world = loadPage();
        await readAndAccept(world, 4);
        await runTimer(world);
        action(world);
        await answer(world, lastFetch(world), 200, kReadDone, {});
        equal(status(world), '', 'status stays clear');
        while (world.timers.length > 0) await runTimer(world);
        equal(polls(world).length, 1, 'no further GET');
        return world;
    });
    test(`${label} while the Read POST is in flight: the late 200 is ignored`, 'FR-2 FR-3', async () => {
        const world = loadPage();
        clickRead(world);
        action(world);
        await answer(world, readPosts(world)[0], 200, 'Reading', { 'Tuner-Seq': '6' });
        equal(status(world), '', 'status stays clear');
        while (world.timers.length > 0) await runTimer(world);
        equal(polls(world).length, 0, 'no GET');
        return world;
    });
}

test('Send stops a running Read poll; only the Send chain polls', 'FR-2 FR-5', async () => {
    const world = loadPage();
    await readAndAccept(world, 1);
    await sendAndAccept(world, 2);
    while (world.timers.length > 0 && polls(world).length === 0) await runTimer(world);
    equal(polls(world).map((entry) => entry.url).join(','), '/tuner/result?seq=2', 'only the Send polled');
    await answer(world, lastFetch(world), 200, 'state=done&b0=400&b1=800&match=144', {});
    equal(status(world), 'Sent\nBit 0: 400 ns set, 400 ns measured\nBit 1: 800 ns set, 800 ns measured\nGRB match 144/144',
          'status');
    while (world.timers.length > 0) await runTimer(world);
    equal(polls(world).length, 1, 'the Read chain never polled');
    return world;
});

test('Read stops a running Send poll; only the Read chain polls', 'FR-2 FR-5', async () => {
    const world = loadPage();
    await sendAndAccept(world, 1);
    await readAndAccept(world, 2);
    while (world.timers.length > 0 && polls(world).length === 0) await runTimer(world);
    equal(polls(world).map((entry) => entry.url).join(','), '/tuner/result?seq=2', 'only the Read polled');
    await answer(world, lastFetch(world), 200, kReadDone, {});
    equal(status(world), kReadSummary, 'status');
    while (world.timers.length > 0) await runTimer(world);
    equal(polls(world).length, 1, 'the Send chain never polled');
    return world;
});

test('Read stops a running Read poll; the summary belongs to the last Read', 'FR-2', async () => {
    const world = loadPage();
    await readAndAccept(world, 1);
    await readAndAccept(world, 2);
    while (world.timers.length > 0 && polls(world).length === 0) await runTimer(world);
    equal(polls(world)[0].url, '/tuner/result?seq=2', 'poll of the last Read');
    await answer(world, polls(world)[0], 200, kReadDone, {});
    equal(status(world), kReadSummary, 'status');
    while (world.timers.length > 0) await runTimer(world);
    equal(polls(world).length, 1, 'GETs');
    equal(readPosts(world).length, 2, 'one POST per press');
    return world;
});

test('a Send poll that receives state=read shows Sent / Measurement not available', 'FR-20', async () => {
    const world = loadPage();
    await sendAndAccept(world, 3);
    await pollOnce(world, kReadDone);
    equal(status(world), 'Sent\nMeasurement not available', 'status');
    equal(world.timers.length, 0, 'read is final: polling stopped');
    return world;
});

test('Send flow is unchanged: 200 without Tuner-Seq still shows Sent / Measurement not available', 'FR-5 SPEC-003', async () => {
    const world = loadPage();
    clickSend(world);
    await answer(world, lastFetch(world), 200, 'Sent', {});
    equal(status(world), 'Sent\nMeasurement not available', 'status');
    return world;
});

test('hostile values in a read body stay text (textContent only), never setInterval', 'FR-4 NFR-10', async () => {
    const world = loadPage();
    await readAndAccept(world, 3);
    await pollOnce(world, 'state=read&b0h=<b>1</b>&b0p=2&b1h=3&b1p=4');
    expect(status(world).includes('<b>1</b>'), 'text not inserted literally');
    equal(world.intervals, 0, 'setInterval calls');
    return world;
});

// ---- Runner ------------------------------------------------------------------------------------------------------

(async () => {
    let failed = 0;
    for (const testCase of cases) {
        try {
            const world = await testCase.body();
            if (world && world.failures.length) throw new Error(world.failures.join('; '));
            console.log(`ok - [${pageName}] ${testCase.name} [${testCase.ids}]`);
        } catch (error) {
            failed++;
            console.log(`not ok - [${pageName}] ${testCase.name} [${testCase.ids}]\n    ${error.message}`);
        }
    }
    console.log(`${cases.length - failed}/${cases.length} read page-script cases passed (${pageName} page, node ${process.version})`);
    process.exit(failed === 0 ? 0 : 1);
})();
