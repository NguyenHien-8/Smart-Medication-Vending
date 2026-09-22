// Test-only ABI declarations for host libcjson.so.1 where development headers
// are unavailable. Firmware uses ESP-IDF's real cJSON.h, never this directory.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cJSON {
 struct cJSON *next, *prev, *child;
 int type;
 char *valuestring;
 int valueint;
 double valuedouble;
 char *string;
} cJSON;
void cJSON_Delete(cJSON*);
void cJSON_DeleteItemFromObjectCaseSensitive(cJSON*, const char*);
cJSON* cJSON_ParseWithLength(const char*, size_t);
cJSON* cJSON_ParseWithLengthOpts(const char*, size_t, const char**, int);
cJSON* cJSON_CreateObject(void);
cJSON* cJSON_CreateArray(void);
cJSON* cJSON_CreateString(const char*);
cJSON* cJSON_Duplicate(const cJSON*, int);
cJSON* cJSON_GetObjectItemCaseSensitive(const cJSON*, const char*);
cJSON* cJSON_AddArrayToObject(cJSON*, const char*);
cJSON* cJSON_AddStringToObject(cJSON*, const char*, const char*);
cJSON* cJSON_AddNumberToObject(cJSON*, const char*, double);
cJSON* cJSON_AddBoolToObject(cJSON*, const char*, int);
int cJSON_AddItemToArray(cJSON*, cJSON*);
int cJSON_AddItemToObject(cJSON*, const char*, cJSON*);
int cJSON_GetArraySize(const cJSON*);
cJSON* cJSON_GetArrayItem(const cJSON*, int);
char* cJSON_PrintUnformatted(const cJSON*);
void cJSON_free(void*);
int cJSON_IsString(const cJSON*);
int cJSON_IsArray(const cJSON*);
int cJSON_IsObject(const cJSON*);
int cJSON_IsNumber(const cJSON*);
int cJSON_IsBool(const cJSON*);
int cJSON_IsNull(const cJSON*);
int cJSON_IsTrue(const cJSON*);
#define cJSON_ArrayForEach(element,array) for ((element) = ((array) != NULL ? (array)->child : NULL); (element) != NULL; (element) = (element)->next)
#ifdef __cplusplus
}
#endif
