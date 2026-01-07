#include <cwist/core/sstring/sstring.h>
#include <cwist/sys/err/cwist_err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

size_t cwist_sstring_get_size(cwist_sstring *str);
int cwist_sstring_compare_sstring(cwist_sstring *left, const cwist_sstring *right);
cwist_error_t cwist_sstring_copy_sstring(cwist_sstring *origin, const cwist_sstring *from);
cwist_error_t cwist_sstring_append_sstring(cwist_sstring *str, const cwist_sstring *from);

cwist_error_t cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len) {
    if (!str) {
      cwist_error_t err = make_error(CWIST_ERR_INT8);
      err.error.err_i8 = ERR_SSTRING_NULL_STRING;
      return err;
    }
    
    char *new_data = (char *)realloc(str->data, len + 1);
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

    char *new_data = (char *)realloc(str->data, new_size + 1);
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

size_t cwist_sstring_get_size(cwist_sstring *str) {
    return str ? str->size : 0;
}

int cwist_sstring_compare_sstring(cwist_sstring *left, const cwist_sstring *right) {
    if (!left || !left->data) {
        if (!right || !right->data) return 0;
        return -1;
    }
    if (!right || !right->data) return 1;
    return strcmp(left->data, right->data);
}

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

cwist_error_t cwist_sstring_trim(cwist_sstring *str) {
    cwist_error_t err = cwist_sstring_rtrim(str);
    if(err.error.err_i8 != ERR_SSTRING_OKAY) return err;
    return cwist_sstring_ltrim(str);
}

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

    char *new_data = (char *)realloc(str->data, new_size + 1); 
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
        char *new_data = (char *)realloc(str->data, data_len + 1);
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
        char *new_data = (char *)realloc(str->data, new_size + 1);
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

cwist_sstring *cwist_sstring_create(void) {
    cwist_sstring *str = (cwist_sstring *)malloc(sizeof(cwist_sstring));
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

void cwist_sstring_destroy(cwist_sstring *str) {
    if (str) {
        if (str->data) free(str->data);
        free(str);
    }
}

int cwist_sstring_compare(cwist_sstring *str, const char *compare_to) {
    if (!str || !str->data) {
        if (!compare_to) return 0; // Both NULL-ish (empty treated as NULL for comparison?)
        return -1; 
    }
    if (!compare_to) return 1; 
    
    return strcmp(str->data, compare_to);
}

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
    
    sub->data = (char *)malloc(length + 1);
    if (!sub->data) {
        cwist_sstring_destroy(sub);
        return NULL;
    }
    sub->size = length;
    
    memcpy(sub->data, str->data + start, length);
    sub->data[length] = '\0';
    
    return sub;
}
