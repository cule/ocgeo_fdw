/*
  Copyright (c) 2019 Stelios Sfakianakis
  
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
  
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "cJSON.h"
#include "sds.h"
#include "ocgeo_api.h"

#ifndef OCGEO_VERSION
#define OCGEO_VERSION "0.3.1"
#endif


#ifndef DEBUG
#define log(...)
#else
#define log(...) elog (DEBUG1, __VA_ARGS__)
#endif

char* ocgeo_version = OCGEO_VERSION;



struct ocgeo_api {
    char* api_key;
    char* server;
};

struct ocgeo_api*
ocgeo_init(const char* api_key, const char* server)
{
    struct ocgeo_api* api = malloc(sizeof(*api));

    api->api_key = sdsnew(api_key);
    api->server = sdsnew(server);
    
    // cJSON_InitHooks(&api->memfns);

    return api;
}

void
ocgeo_close(struct ocgeo_api* api)
{
    if (api == NULL)
        return;
    sdsfree(api->server);
    sdsfree(api->api_key);
    free(api);
}


static ocgeo_latlng_t ocgeo_invalid_point = {.lat = -91.0, .lng=-181};

struct http_response {
    sds data;
};

static size_t
write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct http_response* r = userdata;
    r->data = sdscatlen(r->data, ptr, size*nmemb);
    return nmemb;
}

#define JSON_INT_VALUE(json) ((json) == NULL || cJSON_IsNull(json) ? 0 : (json)->valueint)
#define JSON_OBJ_GET_STR(obj,name) (cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj,name)))
#define JSON_OBJ_GET_INT(obj,name) (cJSON_GetObjectItemCaseSensitive(obj,name)->valueint)

static inline void
parse_latlng(cJSON* json, ocgeo_latlng_t* latlng)
{
    latlng->lat = cJSON_GetObjectItemCaseSensitive(json,"lat")->valuedouble;
    latlng->lng = cJSON_GetObjectItemCaseSensitive(json,"lng")->valuedouble;
}

static int
parse_response_json(cJSON* json, ocgeo_response_t* response)
{
    cJSON* obj = NULL;

    sds url = response->url; /* protect the url from memset */
    memset(response, 0, sizeof(ocgeo_response_t));
    response->url = url;
    response->results = NULL;

    obj = cJSON_GetObjectItemCaseSensitive(json, "status");
    assert(obj);
    response->status.code = JSON_OBJ_GET_INT(obj,"code");
    response->status.message = JSON_OBJ_GET_STR(obj,"message");

    /* Rate information, may not returned (e.g. for paying customers): */
    obj = cJSON_GetObjectItem(json, "rate");
    if (obj) {
        response->rateInfo.limit = JSON_OBJ_GET_INT(obj,"limit");
        response->rateInfo.remaining = JSON_OBJ_GET_INT(obj,"remaining");
        response->rateInfo.reset = JSON_OBJ_GET_INT(obj,"reset");
    }

    obj = cJSON_GetObjectItem(json, "total_results");
    assert(obj);
    response->total_results = JSON_INT_VALUE(obj);
    if (response->total_results <= 0) {
        return 0;
    }

    response->results = malloc(response->total_results * sizeof(ocgeo_result_t));
    obj = cJSON_GetObjectItemCaseSensitive(json, "results");
    assert(obj);

    cJSON* result_js;
    int k = 0;
    ocgeo_result_t proto = {0};
    ocgeo_result_t** pprev = &response->results;
    for (result_js = obj->child; result_js!= NULL; result_js = result_js->next, k++) {
        ocgeo_result_t* result = response->results + k;
        *result = proto; /* initialize with 0/NULL values */
        result->internal = result_js;
        /* keep them in a list for easy traversal */
        (*pprev) = result;
        pprev = &result->next;

        result->confidence = JSON_OBJ_GET_INT(result_js,"confidence");

        cJSON* bounds_js = cJSON_GetObjectItemCaseSensitive(result_js, "bounds");
        // assert(bounds_js);
        if (bounds_js) {
            result->bounds = calloc(1, sizeof(ocgeo_latlng_bounds_t));
            parse_latlng(cJSON_GetObjectItem(bounds_js, "northeast"), &result->bounds->northeast);
            parse_latlng(cJSON_GetObjectItem(bounds_js, "southwest"), &result->bounds->southwest);
        }

        cJSON* geom_js = cJSON_GetObjectItemCaseSensitive(result_js, "geometry");
        // assert(geom_js);
        if (geom_js)
            parse_latlng(geom_js, &result->geometry);
        else
            result->geometry = ocgeo_invalid_point;
    }
    return 0;
}

/*
 * Return true if the given coordinates are "valid":
 *  - Latitude should be between -90.0 and 90.0.
 *  - Longitude should be between -180.0 and 180.0.
*/
static inline
bool ocgeo_is_valid_latlng(ocgeo_latlng_t coords)
{
       return -90.0 <=coords.lat && coords.lat <= 90.0 &&
               -180.0 <=coords.lng && coords.lng <= 180.0;
}

static sds
ocgeo_server_request_uri_impl(const char* api_key, const char* server,
        const char* query, bool is_fwd, ocgeo_params_t* params);

sds
ocgeo_server_request_uri_impl(const char* api_key, const char* server,
        const char* query, bool is_fwd, ocgeo_params_t* params)
{
    // Build URL:
    char* q_escaped = curl_easy_escape(NULL, query, 0);
    sds url = sdsempty();
    url = sdscatprintf(url, "%s?q=%s&key=%s", server, q_escaped, api_key);
    curl_free(q_escaped);
    if (is_fwd && params->countrycode)
        url = sdscatprintf(url, "&countrycode=%s", params->countrycode);
    if (params->language)
        url = sdscatprintf(url, "&language=%s", params->language);
    if (params->limit)
        url = sdscatprintf(url, "&limit=%d", params->limit);
    if (params->min_confidence)
        url = sdscatprintf(url, "&min_confidence=%d", params->min_confidence);
    url = sdscatprintf(url, "&no_annotations=%d", params->no_annotations ? 1 : 0);
    if (params->no_dedupe)
        url = sdscat(url, "&no_dedupe=1");
    if (params->no_record)
        url = sdscat(url, "&no_record=1");
    if (is_fwd && params->roadinfo)
        url = sdscat(url, "&roadinfo=1");
    if (is_fwd && ocgeo_is_valid_latlng(params->proximity))
        url = sdscatprintf(url, "&proximity=%.8F,%.8F", params->proximity.lat, params->proximity.lng);
    return url;
}


static bool
do_request(CURL* curl, bool is_fwd, const char* q, 
           const char* api_key, const char* server,
           ocgeo_params_t* params, ocgeo_response_t* response)
{
    if (params == NULL) {
        ocgeo_params_t params = ocgeo_default_params();
        return do_request(curl, is_fwd, q, api_key, server, &params, response);
    }

    /* Make sure that we have a proper response: */
    if (response == NULL)
        return false;

    // Build URL:
    sds url = ocgeo_server_request_uri_impl(api_key, server, q, is_fwd, params);
    if (response->url) {
        sdsfree(response->url);
        response->url = NULL;
    }
    response->url = url;

    // log("URL=%s\n", url);

    struct http_response r; r.data = sdsempty();
    sds user_agent = sdsempty();
    user_agent = sdscatprintf(user_agent, "ocgeo_fdw/%s (%s)", ocgeo_version, curl_version());
    curl_easy_setopt(curl, CURLOPT_URL, response->url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    sdsfree(user_agent);

    if (res != CURLE_OK) {
        sdsfree(r.data);
        return false;
    }

    cJSON* json = cJSON_Parse(r.data);
    sdsfree(r.data);

    if (json == NULL)
        return false;

    parse_response_json(json, response);
    return true;
}

ocgeo_params_t ocgeo_default_params()
{

    ocgeo_params_t params = {0};
    params.proximity = ocgeo_invalid_point;
    return params;
}


bool
ocgeo_forward(struct ocgeo_api* api, const char* q,
        ocgeo_params_t* params, ocgeo_response_t* response)
{
    CURL* curl = curl_easy_init();
    return do_request(curl, true, q, api->api_key, api->server, params, response);
}

bool
ocgeo_reverse(struct ocgeo_api* api, double lat, double lng, 
        ocgeo_params_t* params, ocgeo_response_t* response)
{
    CURL* curl = curl_easy_init();
    sds q = sdsempty();
    q = sdscatprintf(q, "%.8F,%.8F", lat, lng);
    bool r = do_request(curl, false, q, api->api_key, api->server, params, response);
    sdsfree(q);
    return r;
}


void ocgeo_response_cleanup(struct ocgeo_api* api, ocgeo_response_t* r)
{
    if (r == NULL)
        return;
    sdsfree(r->url);
    r->url = NULL;

    for(ocgeo_result_t* result=r->results; result!=NULL; result=result->next){
        free(result->bounds);
    }
    r->total_results = 0;
    free(r->results);
    r->results = NULL;
}

static
int str_to_maybe_int(sds s)
{
    int k = 0;
    int len = sdslen(s);
    for (int i=0; i<len; ++i) {
        char c = s[i];
        k += 10*k;
        switch(c) {
        case '0': break;
        case '1': k += 1; break;
        case '2': k += 2; break;
        case '3': k += 3; break;
        case '4': k += 4; break;
        case '5': k += 5; break;
        case '6': k += 6; break;
        case '7': k += 7; break;
        case '8': k += 8; break;
        case '9': k += 9; break;
        default:
            return -1;
        }
    }
    return k;
}

static
cJSON* get_json_field(cJSON* parent, const char* path)
{
    static const char* sep = ".";

    if (parent == NULL)
        return NULL;
    int n = 0;
    sds* fields = sdssplitlen(path, strlen(path), sep, 1, &n);
    if (fields == NULL)
        return NULL;

    cJSON* current = parent;
    for (int k = 0; k<n && current != NULL; ++k) {
        int index = str_to_maybe_int(fields[k]);
        if (index >= 0)
            current = cJSON_GetArrayItem(current, index);
        else
            current = cJSON_GetObjectItemCaseSensitive(current, fields[k]);
    }
    sdsfreesplitres(fields, n);
    return current;
}

const char* ocgeo_response_get_str(ocgeo_result_t* r, const char* path, bool* ok)
{
    cJSON* js = get_json_field(r->internal, path);
    if (js == NULL || cJSON_IsNull(js) || !cJSON_IsString(js)) {
        *ok = false;
        return NULL;
    }
    *ok = true;
    return js->valuestring;
}

int ocgeo_response_get_int(ocgeo_result_t* r, const char* path, bool* ok)
{
    cJSON* js = get_json_field(r->internal, path);
    if (js == NULL || cJSON_IsNull(js) || !cJSON_IsNumber(js)) {
        *ok = false;
        return 0;
    }
    *ok = true;
    return js->valueint;
}

double ocgeo_response_get_dbl(ocgeo_result_t* r, const char* path, bool* ok)
{
    cJSON* js = get_json_field(r->internal, path);
    if (js == NULL || cJSON_IsNull(js) || !cJSON_IsNumber(js)) {
        *ok = false;
        return 0.0;
    }
    *ok = true;
    return js->valuedouble;
}
