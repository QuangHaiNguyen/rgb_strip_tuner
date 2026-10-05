/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file updater_page.c
 * @brief Upload page fragments and reason texts of the updater (SPEC-007 FR-24 to FR-26).
 *
 * Self-contained, no external references. The script sends the selected file as the raw body
 * of POST /update (application/octet-stream) with XMLHttpRequest, which reports upload
 * progress (FR-25). Before sending it requests GET /status once: state uploading or done shows
 * `Upload in progress`, a failed request shows `Send failed, check connection`, and in both cases
 * nothing is sent (no retry, no timer). Layout and accessibility follow SPEC-003 NFR-12: viewport meta, lang,
 * labelled input, 44 px touch targets, 16 px input font, dark text on white.
 */
#include "updater_page.h"
#include "fw_meta.h"

/* Reason texts (FR-26). */
#define REASON_TEXT_REQUESTED "Update requested from the device."
#define REASON_TEXT_NO_FIRMWARE "No valid firmware installed."
#define REASON_TEXT_CRASH_LOOP "The firmware crashed repeatedly and was stopped."
#define REASON_TEXT_BOOT_SELECT_FAILED "Could not start the firmware."
#define REASON_TEXT_MAX_LEN (sizeof(REASON_TEXT_CRASH_LOOP) - 1)

#define PAGE_HEAD                                                                                              \
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"                                          \
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"                                  \
    "<title>RGB LED Tuner firmware update</title><style>"                                                      \
    "body{font-family:sans-serif;max-width:30em;margin:0 auto;padding:1em;color:#111;background:#fff}"         \
    "input,button{display:block;width:100%;min-height:44px;font-size:16px;margin:.5em 0;box-sizing:border-box}" \
    "button{background:#0b4fa8;color:#fff;border:0;border-radius:4px}"                                         \
    "</style></head><body><h1>RGB LED Tuner firmware update</h1><p>Installed firmware: "

#define PAGE_MIDDLE "</p><p>"

#define PAGE_TAIL                                                                                              \
    "</p><label for=\"f\">Firmware file (.bin)</label><input id=\"f\" type=\"file\" accept=\".bin\">"          \
    "<button id=\"u\" type=\"button\">Upload</button><p id=\"s\" role=\"status\" aria-live=\"polite\"></p>"   \
    "<script>"                                                                                                 \
    "var s=document.getElementById('s'),u=document.getElementById('u');"                                       \
    "function d(t){s.textContent=t;u.disabled=false;}"                                                         \
    "function p(f){var x=new XMLHttpRequest();"                                                                \
    "x.open('POST','/update');"                                                                                \
    "x.setRequestHeader('Content-Type','application/octet-stream');"                                           \
    "x.upload.onprogress=function(e){if(e.lengthComputable)"                                                   \
    "s.textContent='Uploading... '+Math.floor(e.loaded*100/e.total)+' %';};"                                   \
    "x.onload=function(){d(x.responseText);};"                                                                 \
    "x.onerror=function(){d('Send failed');};"                                                                 \
    "s.textContent='Uploading... 0 %';x.send(f);}"                                                             \
    "u.onclick=function(){"                                                                                    \
    "var f=document.getElementById('f').files[0];"                                                             \
    "if(!f){s.textContent='Select a .bin file';return;}"                                                       \
    "if(!/\\.bin$/i.test(f.name)){s.textContent='Not a .bin file';return;}"                                    \
    "if(f.size>1114112){s.textContent='Invalid size';return;}"                                                \
    "u.disabled=true;var q=new XMLHttpRequest();q.open('GET','/status');"                                      \
    "q.onload=function(){if(q.status!=200)d('Send failed, check connection');"                                 \
    "else if(/state=(uploading|done)/.test(q.responseText))d('Upload in progress');else p(f);};"               \
    "q.onerror=function(){d('Send failed, check connection');};q.send();};"                                    \
    "</script></body></html>"

const char g_updater_page_head[] = PAGE_HEAD;
const char g_updater_page_middle[] = PAGE_MIDDLE;
const char g_updater_page_tail[] = PAGE_TAIL;

#define PAGE_MAX_BYTES                                                                                         \
    ((sizeof(PAGE_HEAD) - 1) + FW_VERSION_LEN + (sizeof(PAGE_MIDDLE) - 1) + REASON_TEXT_MAX_LEN +            \
     (sizeof(PAGE_TAIL) - 1))

_Static_assert(sizeof(REASON_TEXT_REQUESTED) - 1 <= REASON_TEXT_MAX_LEN, "longest reason text");
_Static_assert(sizeof(REASON_TEXT_NO_FIRMWARE) - 1 <= REASON_TEXT_MAX_LEN, "longest reason text");
_Static_assert(sizeof(REASON_TEXT_BOOT_SELECT_FAILED) - 1 <= REASON_TEXT_MAX_LEN, "longest reason text");
_Static_assert(PAGE_MAX_BYTES <= UPDATER_PAGE_MAX_BYTES, "updater page exceeds 3,072 bytes (SPEC-007 FR-24)");

const char *GetUpdaterReasonName(updater_reason_t reason)
{
    switch (reason) {
    case UPDATER_REASON_REQUESTED: return "requested";
    case UPDATER_REASON_NO_FIRMWARE: return "no_firmware";
    case UPDATER_REASON_CRASH_LOOP: return "crash_loop";
    case UPDATER_REASON_BOOT_SELECT_FAILED: return "boot_select_failed";
    }
    return "no_firmware";
}

const char *GetUpdaterReasonText(updater_reason_t reason)
{
    switch (reason) {
    case UPDATER_REASON_REQUESTED: return REASON_TEXT_REQUESTED;
    case UPDATER_REASON_NO_FIRMWARE: return REASON_TEXT_NO_FIRMWARE;
    case UPDATER_REASON_CRASH_LOOP: return REASON_TEXT_CRASH_LOOP;
    case UPDATER_REASON_BOOT_SELECT_FAILED: return REASON_TEXT_BOOT_SELECT_FAILED;
    }
    return REASON_TEXT_NO_FIRMWARE;
}

size_t GetUpdaterPageMaxBytes(void)
{
    return PAGE_MAX_BYTES;
}
