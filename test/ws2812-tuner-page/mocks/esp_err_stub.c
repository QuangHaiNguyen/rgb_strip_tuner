/* Minimal esp_err_to_name() stub: credential_store.c (pulled in transitively by portal_form.c's
 * ValidateSubmission()/IsCredentialsValid()) logs with it, but no tuner test inspects the text. */
#include "esp_err.h"

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ERR";
}
