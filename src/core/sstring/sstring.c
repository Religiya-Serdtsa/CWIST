#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/err/cwist_err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/**
 * @file sstring.c
 * @brief Mutable string helpers used throughout CWIST for request/response and utility text handling.
 */

/** @brief Forward declaration for the method-table size callback. */
size_t cwist_sstring_get_size(cwist_sstring *str);
/** @brief Forward declaration for the method-table compare callback. */
int cwist_sstring_compare_sstring(cwist_sstring *left, const cwist_sstring *right);
/** @brief Forward declaration for the method-table copy callback. */
cwist_error_t cwist_sstring_copy_sstring(cwist_sstring *origin, const cwist_sstring *from);
/** @brief Forward declaration for the method-table append callback. */
cwist_error_t cwist_sstring_append_sstring(cwist_sstring *str, const cwist_sstring *from);
/** @brief Forward declaration for the escaped-append method-table callback. */
cwist_error_t cwist_sstring_append_sstring_escaped(cwist_sstring *str, const cwist_sstring *from);

/**
 * @brief Replace the string contents with an arbitrary byte range.
 * @param str Target string object.
 * @param data Source bytes to copy.
 * @param len Number of source bytes to copy.
 * @return ERR_SSTRING_OKAY on success, or an error payload describing the failure.
 */
cwist_error_t cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len) {
    if (!str) {
      cwist_error_t err = make_error(CWIST_ERR_INT8);
      err.error.err_i8 = ERR_SSTRING_NULL_STRING;
      return err;
    }
    
    char *new_data = (char *)cwist_realloc(str->data, len + 1);
    if (!new_data && len > 0) {
      cwist_error_t err = make_error(CWIST_ERR_JSON);
      err.error.err_json = cJSON_CreateObject();
      cJSON_AddStringToObject(err.error.err_json, "err", "cannot assign string: memory is full");
      return err;
    }
    str->data = new_data;
    str->size = len; 
    if (data && len > 0) memcpy(str->data, data, len);
    if (str->data) str->data[len] = '\0';

    cwist_error_t err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Append an arbitrary byte range to the end of the string.
 * @param str Target string object.
 * @param data Source bytes to append.
 * @param len Number of source bytes to append.
 * @return ERR_SSTRING_OKAY on success, or an error payload describing the failure.
 */
cwist_error_t cwist_sstring_append_len(cwist_sstring *str, const char *data, size_t len) {
    if (!str) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_NULL_STRING;
        return err;
    }
    if (!data || len == 0) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_OKAY;
        return err;
    }

    size_t current_len = str->size;
    size_t new_size = current_len + len;

    char *new_data = (char *)cwist_realloc(str->data, new_size + 1);
    if (!new_data) {
         cwist_error_t err = make_error(CWIST_ERR_JSON);
         err.error.err_json = cJSON_CreateObject();
         cJSON_AddStringToObject(err.error.err_json, "err", "Cannot append: memory full");
         return err;
    }
    str->data = new_data;
    memcpy(str->data + current_len, data, len);
    str->size = new_size;
    str->data[new_size] = '\0';

    cwist_error_t err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Initialize a mutable string with the default append/copy behavior.
 * @param str String object to initialize in caller-owned storage.
 * @return ERR_SSTRING_OKAY on success, or ERR_SSTRING_NULL_STRING for NULL input.
 */
cwist_error_t cwist_sstring_init(cwist_sstring *str) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);
    if (!str) {
        err.error.err_i8 = ERR_SSTRING_NULL_STRING;
        return err;
    }

    str->data = NULL;
    str->size = 0;
    str->is_fixed = false;
    str->get_size = cwist_sstring_get_size;
    str->compare = cwist_sstring_compare_sstring;
    str->copy = cwist_sstring_copy_sstring;
    str->append = cwist_sstring_append_sstring;

    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Initialize a mutable string whose append helper performs HTML escaping.
 * @param str String object to initialize in caller-owned storage.
 * @return ERR_SSTRING_OKAY on success, or ERR_SSTRING_NULL_STRING for NULL input.
 */
cwist_error_t cwist_sstring_init_escaped(cwist_sstring *str) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);
    if (!str) {
        err.error.err_i8 = ERR_SSTRING_NULL_STRING;
        return err;
    }

    str->data = NULL;
    str->size = 0;
    str->is_fixed = false;
    str->get_size = cwist_sstring_get_size;
    str->compare = cwist_sstring_compare_sstring;
    str->copy = cwist_sstring_copy_sstring;
    str->append = cwist_sstring_append_sstring_escaped;

    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Return the cached string size in bytes.
 * @param str String object to inspect.
 * @return Stored size, or 0 when the string is NULL.
 */
size_t cwist_sstring_get_size(cwist_sstring *str) {
    return str ? str->size : 0;
}

/**
 * @brief Compare two CWIST string objects using strcmp semantics.
 * @param left Left-hand string.
 * @param right Right-hand string.
 * @return Negative, zero, or positive depending on lexical ordering.
 */
int cwist_sstring_compare_sstring(cwist_sstring *left, const cwist_sstring *right) {
    if (!left || !left->data) {
        if (!right || !right->data) return 0;
        return -1;
    }
    if (!right || !right->data) return 1;
    return strcmp(left->data, right->data);
}

/**
 * @brief Trim leading ASCII whitespace from the string in place.
 * @param str String object to mutate.
 * @return ERR_SSTRING_OKAY on success, or ERR_SSTRING_NULL_STRING for invalid input.
 */
cwist_error_t cwist_sstring_ltrim(cwist_sstring *str) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SSTRING_NULL_STRING;
    if (!str || !str->data) return err;

    size_t len = strlen(str->data);
    size_t start = 0;
    while (start < len && isspace((unsigned char)str->data[start])) {
        start++;
    }

    if (start > 0) {
        memmove(str->data, str->data + start, len - start + 1);
        str->size -= start; 
    }

    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;                              
}

/**
 * @brief Trim trailing ASCII whitespace from the string in place.
 * @param str String object to mutate.
 * @return ERR_SSTRING_OKAY on success, or an error describing invalid input.
 */
cwist_error_t cwist_sstring_rtrim(cwist_sstring *str) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SSTRING_NULL_STRING;
    if (!str || !str->data) return err; 

    size_t len = strlen(str->data);
    
    if (len == 0) {
      err.error.err_i8 = ERR_SSTRING_ZERO_LENGTH;
      return err;
    }

    size_t end = len - 1;
    while (end < len && 
        isspace((unsigned char)str->data[end])) { 
        end--;
    }
    
    str->data[end + 1] = '\0';
    
    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Trim both leading and trailing ASCII whitespace from the string.
 * @param str String object to mutate.
 * @return ERR_SSTRING_OKAY on success, or the first trim error encountered.
 */
cwist_error_t cwist_sstring_trim(cwist_sstring *str) {
    cwist_error_t err = cwist_sstring_rtrim(str);
    if(err.error.err_i8 != ERR_SSTRING_OKAY) return err;
    return cwist_sstring_ltrim(str);
}

/**
 * @brief Resize the string buffer, optionally allowing truncation.
 * @param str String object to resize.
 * @param new_size Requested new size in bytes.
 * @param blow_data When true, allow shrinking below the current content length.
 * @return ERR_SSTRING_OKAY on success, or an error describing the resize failure.
 */
cwist_error_t cwist_sstring_change_size(cwist_sstring *str, size_t new_size, bool blow_data) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);

    if (!str) {
      err.error.err_i8 = ERR_SSTRING_NULL_STRING;
      return err;
    }

    if (str->is_fixed) {
      err.error.err_i8 = ERR_SSTRING_CONSTANT;
      return err;
    }

    size_t current_len = str->data ? strlen(str->data) : 0;

    if (new_size < current_len && !blow_data) {
        err = make_error(CWIST_ERR_JSON);
        err.error.err_json = cJSON_CreateObject();
        cJSON_AddStringToObject(err.error.err_json, "err", "New size is smaller than current data length and blow_data is false.");
        return err;
    }

    char *new_data = (char *)cwist_realloc(str->data, new_size + 1); 
    if (!new_data && new_size > 0) {
        err.error.err_i8 = ERR_SSTRING_RESIZE_TOO_LARGE;
        return err;                                                
    }

    str->data = new_data;
    str->size = new_size;
    
    // Ensure null termination if growing or if it was just allocated
    if (new_size >= current_len) {
        if (current_len == 0) {
             str->data[0] = '\0';
        }
    } else {
        str->data[new_size] = '\0';
    }

   err.error.err_i8 = ERR_SSTRING_OKAY;
   return err;
}

/**
 * @brief Replace the string contents with a NUL-terminated C string.
 * @param str Target string object.
 * @param data Source string, or NULL to clear the value.
 * @return ERR_SSTRING_OKAY on success, or an error payload describing the failure.
 */
cwist_error_t cwist_sstring_assign(cwist_sstring *str, char *data) {
    if (!str) {
      cwist_error_t err = make_error(CWIST_ERR_INT8);
      err.error.err_i8 = ERR_SSTRING_NULL_STRING;
      return err;
    }
    
    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();

    size_t data_len = data ? strlen(data) : 0;

    if (str->is_fixed) {
        if (data_len > str->size) {
          cJSON_AddStringToObject(err.error.err_json, "err", "string's assigned size is smaller than given data");

          
          cwist_error_t err_resize = cwist_sstring_change_size(str, strlen(str->data), false);
          if(err_resize.error.err_i8 == ERR_SSTRING_OKAY) {
            return err_resize;
          } else {
            return err;
          }
        }
        if (str->data) strcpy(str->data, data ? data : "");
    } else {
        char *new_data = (char *)cwist_realloc(str->data, data_len + 1);
        if (!new_data) {
          cJSON_AddStringToObject(err.error.err_json, "err", "cannot assign string: memory is full");
          return err;
        }
        str->data = new_data;
        str->size = data_len; 
        if (data) strcpy(str->data, data);
        else str->data[0] = '\0';
    }

    cJSON_Delete(err.error.err_json); 
    err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SSTRING_OKAY;

    return err;
}

/**
 * @brief Append a NUL-terminated C string to the target string.
 * @param str Target string object.
 * @param data Source string to append.
 * @return ERR_SSTRING_OKAY on success, or an error payload describing the failure.
 */
cwist_error_t cwist_sstring_append(cwist_sstring *str, const char *data) {
    if (!str) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_NULL_STRING;
        return err;
    }
    if (!data) {
        // Appending nothing is success
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_OKAY;
        return err;
    }

    size_t current_len = str->data ? strlen(str->data) : 0;
    size_t append_len = strlen(data);
    size_t new_size = current_len + append_len;

    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();

    if (str->is_fixed) {
        if (new_size > str->size) {
            cJSON_AddStringToObject(err.error.err_json, "err", "Cannot append: would exceed fixed size");
            return err;
        }
    } else {
        char *new_data = (char *)cwist_realloc(str->data, new_size + 1);
        if (!new_data) {
             cJSON_AddStringToObject(err.error.err_json, "err", "Cannot append: memory full");
             return err;
        }
        str->data = new_data;
        str->size = new_size;
    }

    // Append logic
    if (str->data) {
        // If it was empty/null before, we need to make sure we don't strcat to garbage
        if (current_len == 0) str->data[0] = '\0';
        strcat(str->data, data);
    }

    cJSON_Delete(err.error.err_json);
    err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Append a string while escaping a small HTML-sensitive character set.
 * @param str Target string object.
 * @param data Source string to append in escaped form.
 * @return ERR_SSTRING_OKAY on success, or an error payload describing the failure.
 */
cwist_error_t cwist_sstring_append_escaped(cwist_sstring *str, const char *data) {
    if (!str) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_NULL_STRING;
        return err;
    }
    if (!data) {
        // Appending nothing is success
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_OKAY;
        return err;
    }

    size_t current_len = str->data ? strlen(str->data) : 0;
    size_t input_len = strlen(data);
    size_t new_size = current_len + (input_len * 6) + 1;

    cwist_error_t err = make_error(CWIST_ERR_JSON);
    err.error.err_json = cJSON_CreateObject();

    if (str->is_fixed) {
        if (new_size > str->size) {
            cJSON_AddStringToObject(err.error.err_json, "err", "Cannot append: would exceed fixed size");
            return err;
        }
    } else {
        char *new_data = (char *)cwist_realloc(str->data, new_size + 1);
        if (!new_data) {
             cJSON_AddStringToObject(err.error.err_json, "err", "Cannot append: memory full");
             return err;
        }
        str->data = new_data;
        str->size = new_size;
    }

    char *ptr = str->data + current_len;
    for (size_t i = 0; i < input_len; i++) {
        switch(data[i]) {
            case '<':
                memcpy(ptr, "&lt;", 4);
                ptr += 4;
                break;
            case '>':
                memcpy(ptr, "&gtl", 4);
                ptr += 4;
                break;
            case '&':
                memcpy(ptr, "&amp;", 5);
                ptr += 5;
                break;
            case '"':
                memcpy(ptr, "&quot;", 6);
                ptr += 6;
                break;
            case '\'':
                memcpy(ptr, "&#39;", 5);
                ptr += 5;
                break;
            default:
                *ptr++ = data[i];
                break;   
        }
    }

    *ptr = '\0';
    str->size = (size_t) (ptr - str->data);

    cJSON_Delete(err.error.err_json);
    err = make_error(CWIST_ERR_INT8);
    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Append the contents of one CWIST string onto another.
 * @param str Destination string.
 * @param from Source CWIST string.
 * @return ERR_SSTRING_OKAY on success, or an error describing invalid input.
 */
cwist_error_t cwist_sstring_append_sstring(cwist_sstring *str, const cwist_sstring *from) {
    if (!str) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_NULL_STRING;
        return err;
    }
    if (!from) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_OKAY;
        return err;
    }
    return cwist_sstring_append(str, from->data);
}

/**
 * @brief Append one CWIST string to another while escaping HTML-sensitive characters.
 * @param str Destination string.
 * @param from Source CWIST string.
 * @return ERR_SSTRING_OKAY on success, or an error describing invalid input.
 */
cwist_error_t cwist_sstring_append_sstring_escaped(cwist_sstring *str, const cwist_sstring *from) {
    if(!str) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_NULL_STRING;
        return err;
    }
    if(!from) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_OKAY;
        return err;
    }
    return cwist_sstring_append_escaped(str, from->data);
}

/**
 * @brief Copy a suffix of the string into a caller-provided buffer.
 * @param str Source string object.
 * @param substr Destination C buffer.
 * @param location Starting offset inside the source string.
 * @return ERR_SSTRING_OKAY on success, or an error describing invalid bounds/input.
 */
cwist_error_t cwist_sstring_seek(cwist_sstring *str, char *substr, int location) {
    cwist_error_t err = make_error(CWIST_ERR_INT8);
    if (!str || !str->data || !substr) {
      err.error.err_i8 = ERR_SSTRING_NULL_STRING;
      return err;
    }
    
    size_t len = strlen(str->data);
    if (location < 0 || (size_t)location >= len) {
      err.error.err_i8 = ERR_SSTRING_OUTOFBOUND;
      return err;
    }

    strcpy(substr, str->data + location);
    
    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Copy the entire string into a caller-provided destination buffer.
 * @param origin Source string object.
 * @param destination Destination C buffer.
 * @return ERR_SSTRING_OKAY on success, or an error describing invalid input.
 */
cwist_error_t cwist_sstring_copy(cwist_sstring *origin, char *destination) {

    cwist_error_t err = make_error(CWIST_ERR_INT8);
    if (!origin || !origin->data || !destination) {
      err.error.err_i8 = ERR_SSTRING_NULL_STRING;
      return err;
    }
    
    strcpy(destination, origin->data);
    
    err.error.err_i8 = ERR_SSTRING_OKAY;
    return err;
}

/**
 * @brief Copy one CWIST string into another.
 * @param origin Destination string object.
 * @param from Source string object, or NULL to clear the destination.
 * @return ERR_SSTRING_OKAY on success, or an error describing invalid input.
 */
cwist_error_t cwist_sstring_copy_sstring(cwist_sstring *origin, const cwist_sstring *from) {
    if (!origin) {
        cwist_error_t err = make_error(CWIST_ERR_INT8);
        err.error.err_i8 = ERR_SSTRING_NULL_STRING;
        return err;
    }
    if (!from) {
        return cwist_sstring_assign(origin, NULL);
    }
    return cwist_sstring_assign(origin, from->data);
}

/**
 * @brief Allocate and initialize a new heap-owned CWIST string object.
 * @return Newly allocated string object, or NULL on allocation failure.
 */
cwist_sstring *cwist_sstring_create(void) {
    cwist_sstring *str = (cwist_sstring *)cwist_alloc(sizeof(cwist_sstring));
    if (!str) return NULL;

    memset(str, 0, sizeof(cwist_sstring));
    str->is_fixed = false;
    str->size = 0;
    str->data = NULL; // Initially empty
    str->get_size = cwist_sstring_get_size;
    str->compare = cwist_sstring_compare_sstring;
    str->copy = cwist_sstring_copy_sstring;
    str->append = cwist_sstring_append_sstring;

    return str;
}

/**
 * @brief Destroy a heap-owned CWIST string and its backing buffer.
 * @param str String object to destroy.
 */
void cwist_sstring_destroy(cwist_sstring *str) {
    if (str) {
        if (str->data) cwist_free(str->data);
        cwist_free(str);
    }
}

/**
 * @brief Compare a CWIST string against a raw C string using strcmp semantics.
 * @param str Left-hand CWIST string.
 * @param compare_to Right-hand raw C string.
 * @return Negative, zero, or positive depending on lexical ordering.
 */
int cwist_sstring_compare(cwist_sstring *str, const char *compare_to) {
    if (!str || !str->data) {
        if (!compare_to) return 0; // Both NULL-ish (empty treated as NULL for comparison?)
        return -1; 
    }
    if (!compare_to) return 1; 
    
    return strcmp(str->data, compare_to);
}

/**
 * @brief Create a heap-owned substring spanning the requested range.
 * @param str Source string object.
 * @param start Zero-based starting index.
 * @param length Requested substring length.
 * @return Newly allocated substring, or NULL when the range is invalid or OOM occurs.
 */
cwist_sstring *cwist_sstring_substr(cwist_sstring *str, int start, int length) {
    if (!str || !str->data || start < 0 || length < 0) return NULL;
    
    size_t current_len = strlen(str->data);
    if ((size_t)start >= current_len) return NULL;
    
    // Adjust length if it goes beyond end
    if ((size_t)(start + length) > current_len) {
        length = current_len - start;
    }
    
    cwist_sstring *sub = cwist_sstring_create();
    if (!sub) return NULL;
    
    sub->data = (char *)cwist_alloc(length + 1);
    if (!sub->data) {
        cwist_sstring_destroy(sub);
        return NULL;
    }
    sub->size = length;
    
    memcpy(sub->data, str->data + start, length);
    sub->data[length] = '\0';
    
    return sub;
}
