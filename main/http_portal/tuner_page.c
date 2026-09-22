/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file tuner_page.c
 * @brief Embedded WS2812 tuner page: one self-contained HTML document (SPEC-003).
 *
 * Minified inline CSS/JS (NFR-17); no external reference (NFR-1); kept as a
 * single static const string in flash (.rodata), sent directly by
 * httpd_resp_send() without copying to RAM (NFR-3).
 */
#include "tuner_page.h"

const char g_tuner_page[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<link rel=icon href=\"data:,\"><title>WS2812 tuner</title>"
    "<style>body{font-size:16px;margin:8px}label{display:block;margin-top:8px}"
    "input,button{font-size:16px;min-height:44px;width:100%;box-sizing:border-box}"
    "button{margin-top:8px}input:invalid{border:2px solid #b00}</style>"
    "</head><body><h1>WS2812 timing tuner</h1><a href=/>Back</a><form id=f>"
    "<label for=b0h>Bit 0 pulse width (ns, 100-1200)</label>"
    "<input id=b0h type=number min=100 max=1200 step=25 value=400>"
    "<label for=b0p>Bit 0 period (ns, 800-2000)</label>"
    "<input id=b0p type=number min=800 max=2000 step=25 value=1250>"
    "<label for=b0d>Bit 0 duty (%, 0-100)</label>"
    "<input id=b0d type=number min=0 max=100 step=any value=32.0>"
    "<label for=b1h>Bit 1 pulse width (ns, 100-1200)</label>"
    "<input id=b1h type=number min=100 max=1200 step=25 value=800>"
    "<label for=b1p>Bit 1 period (ns, 800-2000)</label>"
    "<input id=b1p type=number min=800 max=2000 step=25 value=1250>"
    "<label for=b1d>Bit 1 duty (%, 0-100)</label>"
    "<input id=b1d type=number min=0 max=100 step=any value=64.0>"
    "<label for=rst>Reset time (us, 50-800)</label>"
    "<input id=rst type=number min=50 max=800 step=10 value=280>"
    "<p>Low time (period - pulse width) must be at least 100 ns.</p>"
    "<button type=button id=sd>Send</button><button type=reset id=df>Defaults</button>"
    "</form><p id=st role=status></p><script>"
    "const $=id=>document.getElementById(id);"
    "const b0h=$('b0h'),b0p=$('b0p'),b0d=$('b0d'),b1h=$('b1h'),b1p=$('b1p'),b1d=$('b1d'),rst=$('rst'),f=$('f'),st=$('st');"
    "function duty(h,p){return (Math.floor((h*1000+p/2)/p)/10).toFixed(1)}function sync(h,p,d){d.value=duty(+h.value,+p.value)}function high(h,p,d){let v=Math.round(+d.value*+p.value/100/25)*25;"
    "v=Math.min(1200,Math.max(100,v));h.value=v;"
    "sync(h,p,d)}[[b0h,b0p,b0d],[b1h,b1p,b1d]].forEach(a=>{a[0].oninput=()=>sync(a[0],a[1],a[2]);"
    "a[1].oninput=()=>sync(a[0],a[1],a[2]);a[2].onchange=()=>high(a[0],a[1],a[2])});"
    "f.addEventListener('input',()=>{st.textContent=''});f.addEventListener('reset',()=>{st.textContent='';"
    "b0h.setCustomValidity('');b0p.setCustomValidity('');b1h.setCustomValidity('');b1p.setCustomValidity('')});"
    "function ok(h,p){var bad=(+p.value-+h.value)<100;h.setCustomValidity(bad?'x':'');"
    "p.setCustomValidity(bad?'x':'')}function valid(){ok(b0h,b0p);ok(b1h,b1p);"
    "return [b0h,b0p,b1h,b1p,rst].every(i=>i.checkValidity())}$('sd').onclick=()=>{if(!valid()){st.textContent='Invalid values';"
    "return}st.textContent='Sending...';"
    "const body='b0h_ns='+b0h.value+'&b0p_ns='+b0p.value+'&b1h_ns='+b1h.value+'&b1p_ns='+b1p.value+'&rst_us='+rst.value;"
    "fetch('/tuner',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.ok?st.textContent='Sent':r.text().then(t=>{st.textContent=t})).catch(()=>{st.textContent='Send failed, check connection'})}"
    "</script></body></html>";
