#ifndef KINDLE_EPUB_RUNTIME_JSON_HPP
#define KINDLE_EPUB_RUNTIME_JSON_HPP

#include <cjson/cJSON.h>

namespace kindle_epub {

// JSON values and buffers must be released through the same library's API.
struct JsonApi {
    decltype(&cJSON_Delete) deleteValue;
    decltype(&cJSON_free) freeMemory;
    decltype(&cJSON_PrintUnformatted) printUnformatted;
    decltype(&cJSON_CreateString) createString;
    decltype(&cJSON_CreateRaw) createRaw;
    decltype(&cJSON_AddItemToObject) addItemToObject;
    decltype(&cJSON_CreateObject) createObject;
    decltype(&cJSON_AddStringToObject) addStringToObject;
    decltype(&cJSON_AddArrayToObject) addArrayToObject;
    decltype(&cJSON_AddItemToArray) addItemToArray;
    decltype(&cJSON_AddObjectToObject) addObjectToObject;
    decltype(&cJSON_AddRawToObject) addRawToObject;
    decltype(&cJSON_AddTrueToObject) addTrueToObject;
    decltype(&cJSON_AddFalseToObject) addFalseToObject;
    decltype(&cJSON_Parse) parse;
    decltype(&cJSON_GetObjectItemCaseSensitive) getObjectItemCaseSensitive;
    decltype(&cJSON_GetArrayItem) getArrayItem;
    decltype(&cJSON_SetValuestring) setValueString;
};

const JsonApi& jsonApi();

} // namespace kindle_epub
#endif
