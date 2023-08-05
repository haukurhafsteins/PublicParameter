#include <string>
#include <string.h>
#include <map>
#include <list>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "pp.h"
#include "esp_debug_helpers.h"

#define MAX_PUBLIC_PARAMETERS 50
#define MAX_STR 128
#define ID_COUNTER_START 1000
#define POST_WAIT_MS 20

typedef struct
{
    // Configuration part
    struct
    {
        const char *name;
        pp_unit_t unit;
        pp_evloop_t *owner;
        pp_read_cb_t read_cb;
        parameter_type_t type;
        char *format_str;
    } conf;

    // State part
    struct
    {
        std::map<esp_event_loop_handle_t, pp_evloop_t> subscription_list;
        uint64_t change_counter;
        int32_t newstate_id;
        int32_t write_id;
        void *valueptr;
    } state;
} public_parameter_t;

static const char *unit_str[] = {"", "V", "A", "Wh", "lpm", "Kg", "%", "g"};
static public_parameter_t par_list[MAX_PUBLIC_PARAMETERS];
static std::map<std::string, public_parameter_t *> nameToPP;
ESP_EVENT_DEFINE_BASE(PP_EVENTS);
static int32_t event_id_counter = ID_COUNTER_START;

static const char *TAG = "PP";


static bool pp_newstate(public_parameter_t *p, void *data, size_t data_size)
{
    if (p == NULL)
    {
        ESP_LOGE(TAG, "%s: Parameter pointer is NULL", __func__);
        return false;
    }
    if (data == NULL)
    {
        ESP_LOGE(TAG, "%s: Data pointer is NULL", __func__);
        return false;
    }
    if (data_size == 0)
    {
        ESP_LOGE(TAG, "%s: Data size is NULL", __func__);
        return false;
    }
    p->state.change_counter++;
    int size = p->state.subscription_list.size();
    if (size > 0)
    {
        for (auto itc = p->state.subscription_list.begin(); itc != p->state.subscription_list.end(); itc++)
        {
            if (pp_evloop_post(&itc->second, p->state.newstate_id, data, data_size))
                size--;
        }
        return (size == 0); // all sends successful
    }
    return false;
}

static pp_t pp_create(const char *name, pp_evloop_t *evloop, parameter_type_t type, esp_event_handler_t event_write_cb, void *valueptr)
{
    if (name == NULL)
    {
        ESP_LOGE(TAG, "%s: Name is NULL", __func__);
        return NULL;
    }

    if (name[0] == 0)
    {
        ESP_LOGE(TAG, "%s: Name is empty, owner %s", __func__, (evloop == NULL) ? "unknown (no evloop)" : evloop->base);
        return NULL;
    }

    std::string key = name;
    std::map<std::string, public_parameter_t *>::iterator it = nameToPP.find(key);
    if (it != nameToPP.end())
    {
        ESP_LOGW(TAG, "%s: %s exist", __func__, name);
        return it->second;
    }

    int par_list_index;
    for (par_list_index = 0; par_list_index < MAX_PUBLIC_PARAMETERS; par_list_index++)
    {
        if (par_list[par_list_index].conf.name == NULL)
            break;
    }

    if (par_list_index >= MAX_PUBLIC_PARAMETERS)
    {
        ESP_LOGW(TAG, "%s: %s not created, reached maximum %d", __func__, name, MAX_PUBLIC_PARAMETERS);
        return NULL;
    }

    public_parameter_t *p = &par_list[par_list_index];
    nameToPP[key] = p;

    p->conf.name = name;
    p->conf.owner = evloop;
    p->state.change_counter = 0;
    p->state.newstate_id = event_id_counter++;
    p->state.write_id = event_id_counter++;
    p->conf.type = type;
    p->state.valueptr = valueptr;
    //p->conf.tojson = NULL;
    if (event_write_cb)
        pp_event_handler_register(evloop, p->state.write_id, event_write_cb, p);
    return p;
}

//-----------------------------------------------------------------------
// Public stuff
//-----------------------------------------------------------------------
void pp_event_handler_register(pp_evloop_t *evloop, int32_t id, esp_event_handler_t cb, void* p)
{
    if (evloop->loop_handle == NULL)
        ESP_ERROR_CHECK(esp_event_handler_register(evloop->base, id, cb, p));
    else
        ESP_ERROR_CHECK(esp_event_handler_instance_register_with(evloop->loop_handle, evloop->base, id, cb, p, NULL));
}

void pp_event_handler_unregister(pp_evloop_t *evloop, int32_t id, esp_event_handler_t cb)
{
    if (evloop->loop_handle == NULL)
        ESP_ERROR_CHECK(esp_event_handler_unregister(evloop->base, id, cb));
    else
        ESP_ERROR_CHECK(esp_event_handler_unregister_with(evloop->loop_handle, evloop->base, id, cb));
}

bool pp_evloop_post(pp_evloop_t *evloop, int32_t id, void *data, size_t data_size)
{
    if (evloop->loop_handle == NULL)
    {
        esp_err_t err = esp_event_post(evloop->base, id, data, data_size, pdMS_TO_TICKS(POST_WAIT_MS));
        if (ESP_OK != err)
        {
            ESP_LOGE(TAG, "%s: esp_event_post, Receiver is %s - %s", __func__, evloop->base, esp_err_to_name(err));
            return false;
        }
    }
    else
    {
        esp_err_t err = esp_event_post_to(evloop->loop_handle, evloop->base, id, data, data_size, pdMS_TO_TICKS(POST_WAIT_MS));
        if (ESP_OK != err)
        {
            ESP_LOGE(TAG, "%s: esp_event_post_to, Receiver is %s - %s", __func__, evloop->base, esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

bool pp_set_unit(pp_t pp, pp_unit_t unit)
{
    ((public_parameter_t *)pp)->conf.unit = unit;
    return true;
}

pp_unit_t pp_get_unit(pp_t pp)
{
    return ((public_parameter_t *)pp)->conf.unit;
}

const char *pp_unit_to_str(pp_unit_t unit)
{
    return unit_str[unit];
}

pp_unit_t pp_string_to_unit(const char *str)
{
    if (strcmp(str, unit_str[UNIT_V]) == 0)
        return UNIT_V;
    if (strcmp(str, unit_str[UNIT_A]) == 0)
        return UNIT_A;
    if (strcmp(str, unit_str[UNIT_Wh]) == 0)
        return UNIT_Wh;
    if (strcmp(str, unit_str[UNIT_lpm]) == 0)
        return UNIT_lpm;
    if (strcmp(str, unit_str[UNIT_Kg]) == 0)
        return UNIT_Kg;
    if (strcmp(str, unit_str[UNIT_PERCENTAGE]) == 0)
        return UNIT_PERCENTAGE;
    if (strcmp(str, unit_str[UNIT_G]) == 0)
        return UNIT_G;
    return UNIT_NONE;
}

pp_t pp_create_float(const char *name, pp_evloop_t *evloop, esp_event_handler_t event_write_cb, float *valueptr)
{
    return pp_create(name, evloop, TYPE_FLOAT, event_write_cb, valueptr);
}
bool pp_delete(pp_t pp)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    if (p == NULL)
        return false;

    nameToPP.erase(p->conf.name);
    p->conf.name = NULL;
    return true;
}
float pp_get_float_value(pp_t pp)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    if (p == NULL)
        return 0.0f;

    return *((float *)p->state.valueptr);
}
pp_t pp_create_float_array(const char *name, pp_evloop_t *evloop, esp_event_handler_t event_write_cb)
{
    return pp_create(name, evloop, TYPE_FLOAT_ARRAY, event_write_cb, NULL);
}
pp_t pp_create_bool(const char *name, pp_evloop_t *evloop, esp_event_handler_t event_write_cb, bool *valueptr)
{
    return pp_create(name, evloop, TYPE_BOOL, event_write_cb, valueptr);
}
pp_t pp_create_string(const char *name, pp_evloop_t *evloop, esp_event_handler_t event_write_cb/*, tojson_cb_t tojson*/)
{
    pp_t pp = pp_create(name, evloop, TYPE_STRING, event_write_cb, 0);
//    public_parameter_t *p = (public_parameter_t *)pp;
    //p->conf.tojson = tojson;
    return pp;
}
pp_t pp_create_binary(const char *name, pp_evloop_t *evloop, esp_event_handler_t event_write_cb/*, tojson_cb_t tojson*/)
{
    pp_t pp = pp_create(name, evloop, TYPE_BINARY, event_write_cb, NULL);
//    public_parameter_t *p = (public_parameter_t *)pp;
    //p->conf.tojson = tojson;
    return pp;
}

pp_t pp_get(const char *name)
{
    std::string key = name;
    std::map<std::string, public_parameter_t *>::iterator it = nameToPP.find(key);
    if (it != nameToPP.end())
        return (pp_t)it->second;
    ESP_LOGW(TAG, "%s: parameter %s not found", __func__, name);
    return NULL;
}

pp_t pp_subscribe(pp_t pp, pp_evloop_t *evloop, esp_event_handler_t event_cb)
{
    if (pp == NULL)
    {
        ESP_LOGW(TAG, "%s: parameter is NULL", __func__);
        return NULL;
    }
    public_parameter_t *p = (public_parameter_t *)pp;
    if (p->conf.type == TYPE_EXECUTE)
    {
        ESP_LOGW(TAG, "%s: No subscription for execute", __func__);
        return NULL;
    }
    pp_event_handler_register(evloop, p->state.newstate_id, event_cb, p);
    p->state.subscription_list[evloop->loop_handle] = *evloop;
    return pp;
}
pp_t pp_unsubscribe(pp_t pp, pp_evloop_t *evloop, esp_event_handler_t event_cb)
{
    if (pp == NULL)
        return NULL;

    public_parameter_t *p = (public_parameter_t *)pp;
    pp_event_handler_unregister(evloop, p->state.newstate_id, event_cb);
    return pp;
}
bool pp_post_write_bool(pp_t pp, bool value)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    if (p == NULL || p->conf.owner == NULL)
        return false;
    return pp_evloop_post(p->conf.owner, p->state.write_id, (void *)&value, sizeof(bool));
}
bool pp_post_write_float(pp_t pp, float value)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    if (p == NULL || p->conf.owner == NULL)
        return false;
    return pp_evloop_post(p->conf.owner, p->state.write_id, (void *)&value, sizeof(float));
}
bool pp_post_write_string(pp_t pp, const char *str)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    if (p == NULL || p->conf.owner == NULL)
        return false;
    return pp_evloop_post(p->conf.owner, p->state.write_id, (void *)str, strlen(str) + 1);
}

bool pp_post_newstate_binary(pp_t pp, void *bin, size_t size)
{
    public_parameter_t *p = (public_parameter_t *)pp;

    pp_newstate(p, (void *)bin, size);
    return true;
}

bool pp_post_newstate_float(pp_t pp, float f)
{
    public_parameter_t *p = (public_parameter_t *)pp;

    if (p->state.subscription_list.size() > 0)
        pp_newstate(p, &f, sizeof(float));
    return true;
}

bool pp_post_newstate_float_array(pp_t pp, pp_float_array_t *fsrc)
{
    public_parameter_t *p = (public_parameter_t *)pp;

    if (p->state.subscription_list.size() > 0)
        pp_newstate(p, (void *)fsrc, pp_get_float_array_size(fsrc->len));
    return true;
}

void pp_reset_float_array(pp_float_array_t *array)
{
    memset(array->data, 0, sizeof(float) * array->len);
}

void pp_reset_int16_array(pp_int16_array_t *array)
{
    memset(array->data, 0, sizeof(int16_t) * array->len);
}

pp_float_array_t *pp_allocate_float_array(size_t len)
{
    pp_float_array_t *p = (pp_float_array_t *)calloc(1, pp_get_float_array_size(len));
    if (p == 0)
    {
        ESP_LOGE(TAG, "Failed to allocate %d bytes", len);
        esp_backtrace_print(5);
        return NULL;
    }
    p->len = len;
    return p;
}

// pp_float_array_t *pp_allocate_float_array_with_hdr(char* hdr, size_t len)
// {
//     size_t hdr_size = strlen(hdr);
//     uint8_t *p = (uint8_t *)calloc(1, hdr_size + pp_get_float_array_size(len));
//     if (p == 0)
//     {
//         ESP_LOGE(TAG, "Failed to allocate %d bytes", len);
//         esp_backtrace_print(5);
//         return NULL;
//     }
//     pp_float_array_t *fhdr = (pp_float_array_t *)&p[hdr_size];
//     fhdr->len = len;
//     return fhdr;
// }

size_t pp_get_float_array_size(size_t len)
{
    return sizeof(pp_float_array_t) + sizeof(float) * len;
}

void* pp_get_valueptr(pp_t pp)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    return p->state.valueptr;
}

const char* pp_get_name(pp_t pp)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    return p->conf.name;
}

parameter_type_t pp_get_type(pp_t pp)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    return p->conf.type;
}

// tojson_cb_t pp_get_tojson(pp_t pp)
// {
//     public_parameter_t *p = (public_parameter_t *)pp;
//     return p->conf.tojson;
// }

pp_evloop_t *pp_get_owner(pp_t pp)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    return p->conf.owner;
}

int pp_get_subscriptions(pp_t pp)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    return p->state.subscription_list.size();
}

pp_t pp_get_par(int index)
{
   if (index >= MAX_PUBLIC_PARAMETERS)
        return NULL;
    return (pp_t) &par_list[index];
}

bool pp_set_read_cb(pp_t pp, pp_read_cb_t read_cb)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    p->conf.read_cb = read_cb;
    return true;
}

bool pp_read_binary(pp_t pp, void *buf, size_t *bufsize)
{
    public_parameter_t *p = (public_parameter_t *)pp;
    if (p->conf.read_cb == NULL)
        return false;
    return p->conf.read_cb(p, buf, bufsize);
}

