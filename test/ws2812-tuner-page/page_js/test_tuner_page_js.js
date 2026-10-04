#!/usr/bin/env node
/**
 * @file test_tuner_page_js.js
 * @brief SPEC-003 T-21 host part (FR-10, FR-11, FR-13, FR-27, FR-28) and SPEC-005 FR-15: runs the page script that
 *        main/http_portal/tuner_page.c embeds, in Node, against a minimal DOM, a scripted fetch() and a manual
 *        setTimeout() queue. No browser and no network: every request is answered by the test.
 *
 * Usage: node test_tuner_page_js.js <dump_tuner_page executable> provisioning|station
 * Exit code 0 when every case passes, 1 otherwise. Registered with ctest by test/ws2812-tuner-page and
 * test/station-mdns-tuner; when Node is missing, ctest runs `dump_tuner_page --skip` instead (exit 77 = skipped).
 */
'use strict';

const { execFileSync } = require('child_process');

const [dumpExecutable, pageName] = process.argv.slice(2);
if (!dumpExecutable || !['provisioning', 'station'].includes(pageName)) {
    console.error('usage: test_tuner_page_js.js <dump_tuner_page> provisioning|station');
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
        if (attributes.id) {
            elements[attributes.id] = { tag: match[1], attributes };
        }
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
        this.oninput = null;
        this.onclick = null;
    }
    get textContent() { return this._text; }
    set textContent(text) { this._text = String(text); this.textWrites.push(this._text); }
    get innerHTML() { this.world.fail(`innerHTML read on #${this.id}`); return ''; }
    set innerHTML(_) { this.world.fail(`innerHTML written on #${this.id} (FR-13: textContent only)`); }
    set outerHTML(_) { this.world.fail(`outerHTML written on #${this.id}`); }
    insertAdjacentHTML() { this.world.fail(`insertAdjacentHTML on #${this.id}`); }
    setCustomValidity(text) { this.customValidity = String(text); }
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
        const timer = { callback, delay, createdAt: world.now, outstandingFetches: world.pendingFetches().length };
        world.timers.push(timer);
        return world.timerCount;
    };
    world.setInterval = () => { world.intervals++; world.fail('setInterval used (FR-11)'); };
    world.fetch = (url, options) => {
        const entry = { url, options: options || {}, at: world.now };
        entry.promise = new Promise((resolve, reject) => { entry.resolve = resolve; entry.reject = reject; });
        entry.settled = false;
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

function setInput(world, id, value) {
    const input = el(world, id);
    input.value = String(value);
    if (input.oninput) input.oninput({ target: input });
    for (const listener of el(world, 'f').listeners.input || []) listener({ target: input });
}

function setTiming(world, b0h, b0p, b1h, b1p, rst) {
    setInput(world, 'b0h', b0h);
    setInput(world, 'b0p', b0p);
    setInput(world, 'b1h', b1h);
    setInput(world, 'b1p', b1p);
    if (rst !== undefined) setInput(world, 'rst', rst);
}

function pressDefaults(world) {
    for (const listener of el(world, 'f').listeners.reset || []) listener({});
}

function clickSend(world) { el(world, 'sd').onclick({}); }

async function answer(world, entry, status_code, body, headers) {
    entry.settled = true;
    entry.resolve(response(status_code, body, headers));
    await flush();
}

async function failNetwork(world, entry) {
    entry.settled = true;
    entry.reject(new TypeError('Failed to fetch'));
    await flush();
}

/** Run the oldest due timer, advancing the fake clock by its delay. */
async function runTimer(world) {
    const timer = world.timers.shift();
    if (!timer) throw new Error('no timer pending');
    world.now += timer.delay;
    timer.callback();
    await flush();
    return timer;
}

const posts = (world) => world.fetches.filter((entry) => entry.options.method === 'POST');
const polls = (world) => world.fetches.filter((entry) => entry.url.startsWith('/tuner/result'));
const lastFetch = (world) => world.fetches[world.fetches.length - 1];

/** Click Send with the current values and answer the POST with 200 and Tuner-Seq @p seq. */
async function sendAndAccept(world, seq) {
    clickSend(world);
    const post = lastFetch(world);
    await answer(world, post, 200, 'Sent', seq === null ? {} : { 'Tuner-Seq': String(seq) });
    return post;
}

/** Answer the poll the pending timer is about to issue. */
async function pollOnce(world, body, status_code = 200) {
    const before = world.fetches.length;
    await runTimer(world);
    if (world.fetches.length !== before + 1) throw new Error('the timer issued no GET /tuner/result');
    await answer(world, lastFetch(world), status_code, body, {});
    return lastFetch(world);
}

// ---- Cases -----------------------------------------------------------------------------------------------------------

const cases = [];
const test = (name, ids, body) => cases.push({ name, ids, body });

function expect(condition, message) {
    if (!condition) throw new Error(message);
}
function equal(actual, expected, what) {
    if (actual !== expected) {
        throw new Error(`${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    }
}

test('loading the page issues no request and starts no timer; defaults are shown', 'FR-9 FR-11', async () => {
    const world = loadPage();
    await flush();
    equal(world.fetches.length, 0, 'requests on load');
    equal(world.timerCount, 0, 'timers on load');
    equal(el(world, 'b0d').textContent, '32.0', 'bit-0 duty');
    equal(el(world, 'b1d').textContent, '64.0', 'bit-1 duty');
    equal(status(world), '', 'status');
    return world;
});

test('editing inputs and Defaults send nothing and start no timer', 'FR-11', async () => {
    const world = loadPage();
    setTiming(world, 500, 1250, 900, 1250, 300);
    pressDefaults(world);
    setInput(world, 'b0h', 425);
    await flush();
    equal(world.fetches.length, 0, 'requests');
    equal(world.timerCount, 0, 'timers');
    equal(el(world, 'b0h').value, '425', 'slider value after edit');
    return world;
});

for (const [id, values] of [['Z', [125, 800, 175, 1125, 280]], ['AA', [400, 1000, 500, 1250, 280]],
                            ['AB', [500, 1000, 600, 1250, 280]], ['B', [100, 800, 100, 800, 50]],
                            ['D', [1200, 2000, 1000, 2000, 800]]]) {
    test(`V4 vector ${id}: the V4 text is shown and nothing is sent`, 'FR-10 FR-22 T-19', async () => {
        const world = loadPage();
        setTiming(world, ...values);
        clickSend(world);
        await flush();
        equal(status(world), 'Bit 0 duty must be less than bit 1 duty', 'status');
        equal(world.fetches.length, 0, 'requests');
        equal(world.timerCount, 0, 'timers');
        for (const input of ['b0h', 'b0p', 'b1h', 'b1p', 'rst']) {
            equal(el(world, input).customValidity, '', `#${input} is not marked invalid`);
        }
        return world;
    });
}

test('vector Y (both duties shown 15.6) is sent: the exact products decide', 'FR-10', async () => {
    const world = loadPage();
    setTiming(world, 175, 1125, 125, 800, 280);
    equal(el(world, 'b0d').textContent, '15.6', 'bit-0 duty display');
    equal(el(world, 'b1d').textContent, '15.6', 'bit-1 duty display');
    clickSend(world);
    equal(posts(world).length, 1, 'POSTs');
    equal(posts(world)[0].options.body, 'b0h_ns=175&b0p_ns=1125&b1h_ns=125&b1p_ns=800&rst_us=280', 'POST body');
    equal(status(world), 'Sending...', 'status');
    return world;
});

test('a V3 violation shows Invalid values (V1-V3 before V4) and sends nothing', 'FR-10', async () => {
    const world = loadPage();
    setTiming(world, 750, 800, 100, 800, 280);   // bit-0 low 50 ns, also V4-violating
    clickSend(world);
    equal(status(world), 'Invalid values', 'status');
    equal(world.fetches.length, 0, 'requests');
    return world;
});

test('the poll starts only after a 200 with Tuner-Seq, 250 ms after the response', 'FR-27', async () => {
    const world = loadPage();
    clickSend(world);
    equal(world.timerCount, 0, 'no timer while the POST is in flight');
    const post = posts(world)[0];
    equal(post.url, '/tuner', 'POST URL');
    equal(post.options.headers['Content-Type'], 'application/x-www-form-urlencoded', 'content type');
    equal(post.options.body, 'b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280', 'POST body');
    await answer(world, post, 200, 'Sent', { 'Tuner-Seq': '7' });
    equal(status(world), 'Sent', 'status after the 200');
    equal(world.timers.length, 1, 'one timer');
    equal(world.timers[0].delay, 250, 'TUNER_RESULT_POLL_MS');
    equal(polls(world).length, 0, 'no GET before the timer fires');
    await runTimer(world);
    equal(polls(world).length, 1, 'one GET');
    equal(polls(world)[0].url, '/tuner/result?seq=7', 'poll URL');
    equal(polls(world)[0].options.cache, 'no-store', 'poll cache mode');
    equal(polls(world)[0].options.method, undefined, 'poll method (GET)');
    return world;
});

test('done: three summary lines with the sent high times and the measured values', 'FR-28 T-20', async () => {
    const world = loadPage();
    await sendAndAccept(world, 3);
    await pollOnce(world, 'state=done&b0=400&b1=800&match=144');
    equal(status(world), 'Sent\nBit 0: 400 ns set, 400 ns measured\nBit 1: 800 ns set, 800 ns measured\nGRB match 144/144',
          'status');
    equal(world.timers.length, 0, 'polling stopped');
    return world;
});

test('done with match=n/a (vector AC) shows GRB match n/a', 'FR-28', async () => {
    const world = loadPage();
    setTiming(world, 500, 1250, 500, 1000, 280);
    await sendAndAccept(world, 4);
    await pollOnce(world, 'state=done&b0=500&b1=500&match=n/a');
    equal(status(world), 'Sent\nBit 0: 500 ns set, 500 ns measured\nBit 1: 500 ns set, 500 ns measured\nGRB match n/a',
          'status');
    return world;
});

for (const [state, text] of [['timeout', 'Measurement failed: no signal'],
                             ['count_error', 'Measurement failed: bad capture'], ['not_measured', 'Not measured'],
                             ['superseded', 'Superseded by a newer send'], ['unknown', 'Measurement not available']]) {
    test(`final state ${state} shows its fixed text and stops`, 'FR-27 FR-28', async () => {
        const world = loadPage();
        await sendAndAccept(world, 5);
        await pollOnce(world, `state=${state}`);
        equal(status(world), `Sent\n${text}`, 'status');
        equal(world.timers.length, 0, 'no further timer');
        equal(polls(world).length, 1, 'GETs');
        return world;
    });
}

test('8 pending polls, each 250 ms after the previous response, then Measurement not available', 'FR-27 FR-28 T-14', async () => {
    const world = loadPage();
    await sendAndAccept(world, 9);
    for (let poll = 1; poll <= 8; poll++) {
        equal(world.timers.length, 1, `one timer before poll ${poll}`);
        equal(world.timers[0].delay, 250, `delay before poll ${poll}`);
        equal(world.timers[0].outstandingFetches, 0, `timer ${poll} created only after the previous response`);
        await pollOnce(world, 'state=pending');
        if (poll < 8) equal(status(world), 'Sent', `status while pending (${poll})`);
    }
    equal(polls(world).length, 8, 'TUNER_RESULT_POLL_MAX GETs');
    equal(world.timers.length, 0, 'no ninth timer');
    equal(status(world), 'Sent\nMeasurement not available', 'status');
    equal(posts(world).length, 1, 'never re-POSTs');
    return world;
});

test('pending, pending, done finishes early on the final state', 'FR-27', async () => {
    const world = loadPage();
    await sendAndAccept(world, 2);
    await pollOnce(world, 'state=pending');
    await pollOnce(world, 'state=pending');
    await pollOnce(world, 'state=done&b0=401&b1=799&match=143');
    equal(polls(world).length, 3, 'GETs');
    equal(status(world), 'Sent\nBit 0: 400 ns set, 401 ns measured\nBit 1: 800 ns set, 799 ns measured\nGRB match 143/144',
          'status');
    return world;
});

test('a non-200 poll stops polling with Measurement not available', 'FR-27', async () => {
    const world = loadPage();
    await sendAndAccept(world, 2);
    await pollOnce(world, 'Invalid request', 400);
    equal(status(world), 'Sent\nMeasurement not available', 'status');
    equal(world.timers.length, 0, 'no further timer');
    return world;
});

test('a network error during polling stops it with Measurement not available, no POST repeated', 'FR-27 T-21f', async () => {
    const world = loadPage();
    await sendAndAccept(world, 2);
    await runTimer(world);
    await failNetwork(world, lastFetch(world));
    equal(status(world), 'Sent\nMeasurement not available', 'status');
    equal(world.timers.length, 0, 'no further timer');
    equal(posts(world).length, 1, 'POSTs');
    return world;
});

test('a 200 without Tuner-Seq shows Sent and Measurement not available, no poll', 'FR-27', async () => {
    const world = loadPage();
    await sendAndAccept(world, null);
    equal(status(world), 'Sent\nMeasurement not available', 'status');
    equal(world.timerCount, 0, 'timers');
    return world;
});

test('a 400 response shows the server text and starts no poll', 'FR-13 FR-27', async () => {
    const world = loadPage();
    clickSend(world);
    await answer(world, lastFetch(world), 400, 'Bit 0 duty must be less than bit 1 duty', { 'Tuner-Seq': '9' });
    equal(status(world), 'Bit 0 duty must be less than bit 1 duty', 'status');
    equal(world.timerCount, 0, 'timers');
    return world;
});

test('a failed POST shows Send failed, check connection and is not retried', 'FR-11 FR-13', async () => {
    const world = loadPage();
    clickSend(world);
    await failNetwork(world, lastFetch(world));
    equal(status(world), 'Send failed, check connection', 'status');
    equal(world.timerCount, 0, 'timers');
    equal(world.fetches.length, 1, 'requests');
    return world;
});

for (const [label, action] of [['an input edit', (world) => setInput(world, 'b1h', 825)],
                               ['Defaults', (world) => pressDefaults(world)]]) {
    test(`${label} before the poll timer fires clears the status and cancels the poll`, 'FR-13 FR-27 T-21e', async () => {
        const world = loadPage();
        await sendAndAccept(world, 4);
        action(world);
        equal(status(world), '', 'status cleared');
        await runTimer(world);
        equal(polls(world).length, 0, 'no GET after the cancel');
        equal(status(world), '', 'status stays clear');
        return world;
    });
    test(`${label} while a poll is in flight: its late response is ignored`, 'FR-13 FR-27', async () => {
        const world = loadPage();
        await sendAndAccept(world, 4);
        await runTimer(world);
        action(world);
        await answer(world, lastFetch(world), 200, 'state=pending', {});
        equal(status(world), '', 'status stays clear');
        while (world.timers.length > 0) await runTimer(world);   // a stale timer may exist; it must not fetch
        equal(polls(world).length, 1, 'no further GET');
        return world;
    });
    test(`${label} while the POST is in flight: the late 200 is ignored`, 'FR-13 FR-27', async () => {
        const world = loadPage();
        clickSend(world);
        action(world);
        await answer(world, posts(world)[0], 200, 'Sent', { 'Tuner-Seq': '6' });
        equal(status(world), '', 'status stays clear');
        while (world.timers.length > 0) await runTimer(world);
        equal(polls(world).length, 0, 'no GET for the cancelled Send');
        return world;
    });
}

test('a new Send cancels the running poll; the summary belongs to the last Send', 'FR-27 T-21a', async () => {
    const world = loadPage();
    await sendAndAccept(world, 1);
    setTiming(world, 425, 1250, 825, 1250, 280);
    await sendAndAccept(world, 2);
    // Two timers exist now: the stale one of Send 1 and the live one of Send 2.
    const first = await runTimer(world);
    equal(first.delay, 250, 'stale timer delay');
    const issued = polls(world).map((entry) => entry.url);
    expect(!issued.includes('/tuner/result?seq=1'), 'the cancelled chain polled seq 1');
    while (polls(world).length === 0) await runTimer(world);
    equal(polls(world)[0].url, '/tuner/result?seq=2', 'poll of the last Send');
    await answer(world, polls(world)[0], 200, 'state=done&b0=425&b1=825&match=144', {});
    equal(status(world), 'Sent\nBit 0: 425 ns set, 425 ns measured\nBit 1: 825 ns set, 825 ns measured\nGRB match 144/144',
          'status');
    equal(posts(world).length, 2, 'one POST per Send');
    return world;
});

test('each Send press makes exactly one POST', 'FR-11 T-14', async () => {
    const world = loadPage();
    for (let press = 1; press <= 3; press++) {
        clickSend(world);
        equal(posts(world).length, press, `POSTs after press ${press}`);
    }
    return world;
});

test('the page uses textContent only and never setInterval', 'FR-13 FR-28 NFR-11', async () => {
    const world = loadPage();
    await sendAndAccept(world, 3);
    await pollOnce(world, 'state=done&b0=<b>1</b>&b1=2&match=3');   // hostile text stays text
    expect(status(world).includes('<b>1</b>'), 'text was not inserted literally');
    equal(world.intervals, 0, 'setInterval calls');
    return world;
});

// ---- Runner ------------------------------------------------------------------------------------------------------

(async () => {
    let failed = 0;
    for (const testCase of cases) {
        let world = null;
        try {
            world = await testCase.body();
            if (world && world.failures.length) throw new Error(world.failures.join('; '));
            console.log(`ok - [${pageName}] ${testCase.name} [${testCase.ids}]`);
        } catch (error) {
            failed++;
            console.log(`not ok - [${pageName}] ${testCase.name} [${testCase.ids}]\n    ${error.message}`);
        }
    }
    console.log(`${cases.length - failed}/${cases.length} page-script cases passed (${pageName} page, node ${process.version})`);
    process.exit(failed === 0 ? 0 : 1);
})();
