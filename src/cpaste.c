#include <kore/kore.h>
#include <kore/http.h>

#include <inih/ini.h>

#include <stddef.h>
#include <stdlib.h>

/*
 * Paste ID generator
 */

static char* const _id_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
static uint16_t const _id_chars_lim = 35;

static char* const
cpaste_gen_id(uint16_t len)
{
    char* result = malloc((len + 1) * sizeof(char));
    result[len] = 0;

    for(uint16_t charN = 0; charN <= len; ++charN)
    {
        result[charN] = (rand() % _id_chars_lim);
    }

    return (char* const) result;
}

/*
 * Paste Intake
 */

struct s_paste
{
    char* const id;
    char* const file;
}

//-------------------------------------------------------------------------------------------------

/*
 * KORE
 */

/*
 * static   /
 */
int
cpaste_kore_main(struct http_request* req)
{
    if (req->method != HTTP_METHOD_POST) 
    {
    	http_response(req, 200, NULL, 0);
    	return (KORE_RESULT_OK);
    }

    http_populate_multipart_form(req);

       
}

/*
 * dynamic /[A-Za-z0-9]+$
 */
int
cpaste_kore_view_paste(struct http_request* req)
{
    return KORE_RESULT_OK;
}
