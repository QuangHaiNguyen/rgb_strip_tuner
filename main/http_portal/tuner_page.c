/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file tuner_page.c
 * @brief Embedded WS2812 tuner page: one self-contained HTML document (SPEC-003).
 *
 * Two high-time sliders with value text, read-only duty, and one CSS-box pulse
 * drawing per bit (FR-7, FR-8). The Defaults button stays a native reset button;
 * its reset handler writes each input's defaultValue itself before redrawing,
 * because the native restore only happens after the reset event (FR-9).
 *
 * Minified inline CSS/JS (NFR-17); no external reference (NFR-1); kept as a
 * single static const string in flash (.rodata), sent directly by
 * httpd_resp_send() without copying to RAM (NFR-3).
 *
 * SPEC-005 FR-15: the station-mode variant g_tuner_page_station is built from the
 * same fragment macros at compile time and differs only by the missing Back link,
 * so the two pages cannot drift apart.
 */
#include "tuner_page.h"

/** @brief Page start up to and including the heading. */
#define TUNER_PAGE_HEAD \
    "<!doctype html><html lang=en><head><meta charset=utf-8>" \
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">" \
    "<link rel=icon href=\"data:,\"><title>WS2812 tuner</title>" \
    "<style>body{font-size:16px;margin:8px}label{display:block;margin-top:8px}" \
    "input,button{font-size:16px;min-height:44px;width:100%;box-sizing:border-box}" \
    "button{margin-top:8px}input:invalid{border:2px solid #b00}" \
    ".r{display:flex;align-items:center}.r span{margin-left:8px;white-space:nowrap}" \
    ".w{display:flex;height:24px}.w i{border:2px solid;border-bottom:0;box-sizing:border-box}" \
    ".w b{flex:1;border-bottom:2px solid}</style>" \
    "</head><body><h1>WS2812 timing tuner</h1>"

/** @brief Link back to the provisioning page (18 bytes); provisioning profile only. */
#define TUNER_PAGE_BACK_LINK "<a href=/>Back</a>"

/** @brief Page rest: the form, the status line and the script. */
#define TUNER_PAGE_BODY \
    "<form id=f>" \
    "<label for=b0h>Bit 0 high time (ns, 100-1200)</label>" \
    "<div class=r><input id=b0h type=range min=100 max=1200 step=25 value=400><span id=b0v></span></div>" \
    "<label for=b0p>Bit 0 period (ns, 800-2000)</label>" \
    "<input id=b0p type=number min=800 max=2000 step=25 value=1250>" \
    "<p>Bit 0 duty: <span id=b0d></span> %</p>" \
    "<div class=w aria-hidden=true><i id=b0w></i><b></b></div>" \
    "<label for=b1h>Bit 1 high time (ns, 100-1200)</label>" \
    "<div class=r><input id=b1h type=range min=100 max=1200 step=25 value=800><span id=b1v></span></div>" \
    "<label for=b1p>Bit 1 period (ns, 800-2000)</label>" \
    "<input id=b1p type=number min=800 max=2000 step=25 value=1250>" \
    "<p>Bit 1 duty: <span id=b1d></span> %</p>" \
    "<div class=w aria-hidden=true><i id=b1w></i><b></b></div>" \
    "<label for=rst>Reset time (us, 50-800)</label>" \
    "<input id=rst type=number min=50 max=800 step=10 value=280>" \
    "<p>Low time (period - high time) must be at least 100 ns.</p>" \
    "<button type=button id=sd>Send</button><button type=reset id=df>Defaults</button>" \
    "</form><p id=st role=status></p><script>" \
    "const $=id=>document.getElementById(id);" \
    "const b0h=$('b0h'),b0p=$('b0p'),b1h=$('b1h'),b1p=$('b1p'),rst=$('rst'),f=$('f'),st=$('st'),all=[b0h,b0p,b1h,b1p,rst];" \
    "function show(n){const h=+$('b'+n+'h').value,p=+$('b'+n+'p').value;$('b'+n+'v').textContent=h+' ns';" \
    "$('b'+n+'d').textContent=(Math.floor((h*1000+p/2)/p)/10).toFixed(1);" \
    "$('b'+n+'w').style.width=Math.min(100,h*100/p)+'%'}" \
    "[0,1].forEach(n=>{$('b'+n+'h').oninput=$('b'+n+'p').oninput=()=>show(n);show(n)});" \
    "f.addEventListener('input',()=>{st.textContent=''});f.addEventListener('reset',()=>{st.textContent='';" \
    "all.forEach(i=>{i.value=i.defaultValue;i.setCustomValidity('')});show(0);show(1)});" \
    "function ok(h,p){var bad=(+p.value-+h.value)<100;h.setCustomValidity(bad?'x':'');" \
    "p.setCustomValidity(bad?'x':'')}function valid(){ok(b0h,b0p);ok(b1h,b1p);" \
    "return all.every(i=>i.checkValidity())}$('sd').onclick=()=>{if(!valid()){st.textContent='Invalid values';" \
    "return}st.textContent='Sending...';" \
    "const body='b0h_ns='+b0h.value+'&b0p_ns='+b0p.value+'&b1h_ns='+b1h.value+'&b1p_ns='+b1p.value+'&rst_us='+rst.value;" \
    "fetch('/tuner',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.ok?st.textContent='Sent':r.text().then(t=>{st.textContent=t})).catch(()=>{st.textContent='Send failed, check connection'})}" \
    "</script></body></html>"

const char g_tuner_page[] = TUNER_PAGE_HEAD TUNER_PAGE_BACK_LINK TUNER_PAGE_BODY;

const char g_tuner_page_station[] = TUNER_PAGE_HEAD TUNER_PAGE_BODY;

/* SPEC-005 FR-15 / SPEC-003 NFR-2: the pages differ by exactly the Back link, and each fits 4,096 bytes. */
_Static_assert(sizeof(g_tuner_page_station) == sizeof(g_tuner_page) - (sizeof(TUNER_PAGE_BACK_LINK) - 1),
               "station page must equal the tuner page minus the Back link");
_Static_assert(sizeof(TUNER_PAGE_BACK_LINK) - 1 == 18, "Back link element must be 18 bytes");
_Static_assert(sizeof(g_tuner_page) - 1 <= 4096, "tuner page exceeds 4,096 bytes");
_Static_assert(sizeof(g_tuner_page_station) - 1 <= 4096, "station tuner page exceeds 4,096 bytes");
