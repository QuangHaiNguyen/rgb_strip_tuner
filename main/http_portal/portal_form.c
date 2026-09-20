/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file portal_form.c
 * @brief Pure helpers: form decoding, JSON escaping and submission validation.
 */
#include "http_portal.h"
#include <string.h>

#define FORM_NAME_MAX (16)

static int GetHexValue(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

bool DecodeFormValue(const char *input, size_t input_length, char *output, size_t output_size)
{
    size_t output_length = 0;
    for (size_t index = 0; index < input_length; ++index) {
        char value = input[index];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (index + 2 >= input_length) {
                return false;
            }
            int high = GetHexValue(input[index + 1]);
            int low = GetHexValue(input[index + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = (char)((high << 4) | low);
            index += 2;
        }
        if (value == '\0' || output_length + 1 >= output_size) {
            return false;
        }
        output[output_length++] = value;
    }
    if (output_size == 0) {
        return false;
    }
    output[output_length] = '\0';
    return true;
}

bool GetFormField(const char *body, const char *name, char *value, size_t value_size)
{
    size_t name_length = strlen(name);
    if (name_length == 0 || name_length >= FORM_NAME_MAX) {
        return false;
    }

    const char *field = body;
    while (*field != '\0') {
        const char *end = strchr(field, '&');
        size_t field_length = (end == NULL) ? strlen(field) : (size_t)(end - field);
        if (field_length > name_length && strncmp(field, name, name_length) == 0 && field[name_length] == '=') {
            return DecodeFormValue(field + name_length + 1, field_length - name_length - 1, value, value_size);
        }
        if (end == NULL) {
            break;
        }
        field = end + 1;
    }
    return false;
}

size_t EscapeJsonString(const char *input, char *output, size_t output_size)
{
    static const char hex_digits[] = "0123456789abcdef";
    size_t length = 0;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; ++cursor) {
        unsigned char character = *cursor;
        bool needs_unicode = character < 0x20 || character == '<' || character == '>' ||
                             character == '&' || character == '\'';
        size_t needed = needs_unicode ? 6 : ((character == '"' || character == '\\') ? 2 : 1);
        if (length + needed + 1 > output_size) {
            return 0;
        }
        if (needs_unicode) {
            memcpy(&output[length], "\\u00", 4);
            output[length + 4] = hex_digits[character >> 4];
            output[length + 5] = hex_digits[character & 0x0F];
        } else if (needed == 2) {
            output[length] = '\\';
            output[length + 1] = (char)character;
        } else {
            output[length] = (char)character;
        }
        length += needed;
    }
    if (output_size == 0) {
        return 0;
    }
    output[length] = '\0';
    return length;
}

bool ValidateSubmission(const wifi_credentials_t *credentials, const wifi_scan_entry_t *entries, uint16_t count)
{
    if (!IsCredentialsValid(credentials)) {
        return false;
    }

    bool is_password_empty = (credentials->password[0] == '\0');
    for (uint16_t index = 0; index < count; ++index) {
        if (strcmp(entries[index].ssid, credentials->ssid) == 0) {
            if (entries[index].is_unsupported) {
                return false;
            }
            return entries[index].is_open ? is_password_empty : !is_password_empty;
        }
    }
    /* Not in the last scan (e.g. dropped by a rescan): only a password proves the intent. */
    return !is_password_empty;
}
