/*
  +----------------------------------------------------------------------+
  | PHP Version 5-7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2016 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:Coco.Fu                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_group_apm.h"

/* If you declare any globals in php_group_apm.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(group_apm)
*/

/* True global resources - no need for thread safety here */
static int le_group_apm;

#define MICRO_IN_SEC 1000000.00
#define MIN_SEC 0.001
#define BUFF_SIZE 512
#define GROUPAPM_VERSION "v0.0.1"

typedef struct group_entry 
{
    double time_of_start;

    char   *func;

    int id;

    struct group_entry      *prev_entry;

} group_entry;

typedef struct group_globals_quotas 
{   
  int    enabled;

  int    id;

  double time_of_minit;

  double time_of_mshutdown;

  int    cpu_num;
#if PHP_VERSION_ID < 70000
  zval   *core_detail;
#else
  zval   core_detail;
#endif
  struct group_entry *prev_entry;

  struct group_entry *entry_free_list;

} group_globals_quotas;

static group_globals_quotas       group_quotas;

static double _phpgettimeofday()
{
    zend_bool get_as_float = 0;
    struct timeval tp = {0};

    if (gettimeofday(&tp, NULL)) {
        return 0;
    }

    return (double)(tp.tv_sec + tp.tv_usec / MICRO_IN_SEC);
}

/**
 * Takes an input of the form /a/b/c/d/foo.php and returns
 * a pointer to one-level directory and basefile name
 * (d/foo.php) in the same string.
 */
static const char *group_get_base_filename(const char *filename) {
  const char *ptr;
  int   found = 0;

  if (!filename)
    return "";

  /* reverse search for "/" and return a ptr to the next char */
  for (ptr = filename + strlen(filename) - 1; ptr >= filename; ptr--) {
    if (*ptr == '/') {
      found++;
    }
    if (found == 2) {
      return ptr + 1;
    }
  }

  /* no "/" char found, so return the whole string */
  return filename;
}

static void (*_zend_execute_ex) (zend_execute_data *execute_data TSRMLS_DC);

#if PHP_VERSION_ID < 70000
static void (*_zend_execute_internal) (zend_execute_data *execute_data,
  struct _zend_fcall_info *fci, int ret TSRMLS_DC);
#else
static void (*_zend_execute_internal) (zend_execute_data *execute_data, zval *return_value);
#endif
static inline const char *group_get_executed_filename(TSRMLS_D)
{ 
  zend_execute_data *ex = EG(current_execute_data);

#if PHP_VERSION_ID < 70000
  if (ex && ex->op_array) {
    return ex->op_array->filename;
  } else {
    return zend_get_executed_filename(TSRMLS_C);
  }
#else
  while (ex && (!ex->func || !ZEND_USER_CODE(ex->func->type))) {
    ex = ex->prev_execute_data;
  }
  if (ex) {
    return ZSTR_VAL(ex->func->op_array.filename);
  } else {
    return zend_get_executed_filename();
  }
#endif
}

#if PHP_VERSION_ID < 70000
static char *group_get_function_name(zend_op_array *ops TSRMLS_DC) {
  const char        *func = NULL;
  const char        *cls = NULL;
  char              *ret = NULL;
  int                len;
  zend_function      *curr_func;

  const char *space = "";
  const char *class_name = "";
  const char *function;
  int origin_len;
  char *origin;
  char *message;
  int is_function = 0;
  if (EG(current_execute_data) &&
        EG(current_execute_data)->opline &&
        EG(current_execute_data)->opline->opcode == ZEND_INCLUDE_OR_EVAL
  ) {
    switch (EG(current_execute_data)->opline->extended_value) {
      case ZEND_EVAL:
        function = "eval";
        is_function = 1;
        break;
      case ZEND_INCLUDE:
        function = "include";
        is_function = 1;
        break;
      case ZEND_INCLUDE_ONCE:
        function = "include_once";
        is_function = 1;
        break;
      case ZEND_REQUIRE:
        function = "require";
        is_function = 1;
        break;
      case ZEND_REQUIRE_ONCE:
        function = "require_once";
        is_function = 1;
        break;
      default:
        function = "Unknown";
    }

    if (is_function){
      const char *filename;
      int   len;
      filename = group_get_base_filename((EG(current_execute_data)->function_state.function->op_array).filename);
      len      = strlen("run_init") + strlen(filename) + 3;
      ret      = (char *)emalloc(len);
      snprintf(ret, len, "run_init::%s", filename);
    } else {
      ret = estrdup(function);
    }
    return ret;
  } else {
    function = get_active_function_name(TSRMLS_C);
    if (!function || !strlen(function)) {
      function = "{main}";
    } else {
      is_function = 1;
      class_name = get_active_class_name(&space TSRMLS_CC);
    }
  }

  if (is_function) {
    origin_len = spprintf(&origin, 0, "%s%s%s", class_name, space, function);
  } else {
    origin_len = spprintf(&origin, 0, "%s", function);
  }

  const char *filename;
  unsigned int line;
  filename = group_get_executed_filename(TSRMLS_C);
  line = zend_get_executed_lineno(TSRMLS_C);

  spprintf(&message, 0, "%s=>%s:%d", origin, filename, line);
  efree(origin);
  ret = estrdup(message);
  efree(message);
  // if (EG(current_execute_data) 
  //   && EG(current_execute_data)->prev_execute_data 
  //   && EG(current_execute_data)->prev_execute_data->function_state.arguments) {
  //   zend_execute_data *real_execute_data = EG(current_execute_data)->prev_execute_data;
    
  //   void **p = real_execute_data->function_state.arguments;
  //   int arg_count = (int) (zend_uintptr_t) * p;
  //   zval *argument_element;
  //   int i;

  //   for (i = 0; i < arg_count; i++) {
  //       argument_element = *(p - (arg_count - i));
  //       php_var_dump(&argument_element, 1);
  //   }

  // }
  return ret;
}
#else
static char *group_get_function_name(zend_op_array *ops TSRMLS_DC) {
  const char        *func = NULL;
  const char        *cls = NULL;
  char              *ret = NULL;
  int                len;
  zend_function      *curr_func;

  const char *space = "";
  const char *class_name = "";
  const char *function;
  int origin_len;
  char *origin;
  char *message;
  int is_function = 0;

  if (!EG(current_execute_data)->func || !ZEND_USER_CODE(EG(current_execute_data)->func->common.type) || EG(current_execute_data)->opline->opcode != ZEND_INCLUDE_OR_EVAL) {
      function = get_active_function_name();
      if (!function || !strlen(function)) {
        function = "{main}";
      } else {
        is_function = 1;
        class_name = get_active_class_name(&space);
      }
  } else {
      switch (EG(current_execute_data)->opline->extended_value) {
        case ZEND_EVAL:
          function = "eval";
          is_function = 1;
          break;
        case ZEND_INCLUDE:
          function = "include";
          is_function = 1;
          break;
        case ZEND_REQUIRE:
          function = "require";
          is_function = 1;
          break;
        case ZEND_INCLUDE_ONCE:
          function = "include_once";
          is_function = 1;
          break;
        case ZEND_REQUIRE_ONCE:
          function = "require_once";
          is_function = 1;
          break;
        default:
          function = "Unknown";
          break;
      }

      if (is_function){
        const char *filename;
        int   len;
        filename = group_get_base_filename(ZSTR_VAL((EG(current_execute_data)->func->op_array).filename));
        len      = strlen("run_init") + strlen(filename) + 3;
        ret      = (char *)emalloc(len);
        snprintf(ret, len, "run_init::%s", filename);
      } else {
        ret = estrdup(function);
      }
      return ret;
  }

  if (is_function) {
    origin_len = spprintf(&origin, 0, "%s%s%s", class_name, space, function);
  } else {
    origin_len = spprintf(&origin, 0, "%s", function);
  }

  const char *filename;
  unsigned int line;
  filename = group_get_executed_filename(TSRMLS_C);
  line = zend_get_executed_lineno();

  spprintf(&message, 0, "%s=>%s:%d", origin, filename, line);
  efree(origin);
  ret = estrdup(message);
  efree(message);

  return ret;
}
#endif

#if PHP_VERSION_ID < 70000
static void record(group_entry **entry  TSRMLS_DC) {

  group_entry   *curr = (*entry);
  double        end_time;
  zval          *detail;

  end_time = _phpgettimeofday();
  if (curr->prev_entry) {

    double end;

    end = end_time - curr->time_of_start;

    if (MIN_SEC < end) {
      MAKE_STD_ZVAL(detail);

      array_init(detail);
      add_next_index_zval(group_quotas.core_detail, detail);

      add_assoc_double(detail, "t", end);
      add_assoc_string(detail, "cf", curr->func, 1);
      add_assoc_long(detail, "id", curr->id);
      add_assoc_long(detail, "pf_id", curr->prev_entry->id);
    }
  }
}
#else
static void record(group_entry **entry  TSRMLS_DC) {

  group_entry   *curr = (*entry);
  double        end_time;
  zval          detail;

  end_time = _phpgettimeofday();
  if (curr->prev_entry) {

    double end;

    end = end_time - curr->time_of_start;

    if (MIN_SEC < end) {
      array_init(&detail);
      add_next_index_zval(&group_quotas.core_detail, &detail);

      add_assoc_double(&detail, "t", end);
      add_assoc_string(&detail, "cf", curr->func);
      add_assoc_long(&detail, "id", curr->id);
      add_assoc_long(&detail, "pf_id", curr->prev_entry->id);
    }
  }
}
#endif 

#define group_begin(entry, fun)\
  do {\
    double        start_time;\
    start_time = _phpgettimeofday();\
    group_entry *curr;\
    curr = group_quotas.entry_free_list;\
    if (curr) {\
      group_quotas.entry_free_list = curr->prev_entry;\
    } else {\
      curr = (group_entry *)malloc(sizeof(group_entry));\
    }\
    curr->func = fun;\
    curr->id = group_quotas.id;\
    curr->time_of_start = start_time;\
    curr->prev_entry = *entry;\
    *entry = curr;\
    group_quotas.id++;\
  } while (0)

#define group_end(entry)\
  do {\
    record(entry TSRMLS_CC);\
    group_entry *curr;\
    curr = *entry;\
    (*entry) = (*entry)->prev_entry;\
    curr->prev_entry = group_quotas.entry_free_list;\
    group_quotas.entry_free_list = curr;\
  } while (0)


ZEND_DLEXPORT void group_execute_ex (zend_execute_data *execute_data TSRMLS_DC) {
#if PHP_VERSION_ID < 70000
  zend_op_array *ops = execute_data->op_array;
#else
  zend_op_array *ops = &execute_data->func->op_array;
#endif
  char          *func = NULL;
  func = group_get_function_name(ops TSRMLS_DC);

  if (!func) {
    execute_ex(execute_data TSRMLS_CC);
    return;
  }

  if (group_quotas.enabled) {
    group_begin(&group_quotas.prev_entry, func);
  }

  execute_ex(execute_data TSRMLS_CC);

  if (group_quotas.enabled && group_quotas.prev_entry) {

    group_end(&group_quotas.prev_entry);

  }
  efree(func);
}

#if PHP_VERSION_ID < 70000
ZEND_DLEXPORT void group_execute_internal (zend_execute_data *execute_data,
  struct _zend_fcall_info *fci, int ret TSRMLS_DC) {

  zend_op_array *ops = execute_data->op_array;

  char          *func = NULL;
  func = group_get_function_name(ops TSRMLS_DC);

  if (!func) {
    execute_internal(execute_data, fci, ret TSRMLS_CC);
    return;
  }

  if (group_quotas.enabled) {
    group_begin(&group_quotas.prev_entry, func);
  }

  execute_internal(execute_data, fci, ret TSRMLS_CC);

  if (group_quotas.enabled && group_quotas.prev_entry) {

    group_end(&group_quotas.prev_entry);

  }
  efree(func);
}
#else
ZEND_DLEXPORT void group_execute_internal (zend_execute_data *execute_data,
  zval *return_value) {

  zend_op_array *ops = &execute_data->func->op_array;

  char          *func = NULL;
  func = group_get_function_name(ops TSRMLS_DC);

  if (!func) {
      execute_internal(execute_data, return_value);
      return;
    }

  if (group_quotas.enabled) {
    group_begin(&group_quotas.prev_entry, func);
  }

  execute_internal(execute_data, return_value);

  if (group_quotas.enabled && group_quotas.prev_entry) {

    group_end(&group_quotas.prev_entry);

  }
  efree(func);
}
#endif

#if PHP_VERSION_ID < 70000
static void begin_watch(TSRMLS_D) {

  _zend_execute_ex = zend_execute_ex;
  zend_execute_ex  = group_execute_ex;

  _zend_execute_internal = zend_execute_internal;
  zend_execute_internal = group_execute_internal;

  if (group_quotas.core_detail) {
    zval_dtor(group_quotas.core_detail);
    FREE_ZVAL(group_quotas.core_detail);
  }

  MAKE_STD_ZVAL(group_quotas.core_detail);
  array_init(group_quotas.core_detail);

  group_quotas.enabled = 1;

  group_quotas.time_of_minit = _phpgettimeofday();

  group_begin(&group_quotas.prev_entry, "{main}");
}
#else
static void begin_watch(TSRMLS_D) {

  _zend_execute_ex = zend_execute_ex;
  zend_execute_ex  = group_execute_ex;

  _zend_execute_internal = zend_execute_internal;
  zend_execute_internal = group_execute_internal;

  ZVAL_NULL(&group_quotas.core_detail);
  array_init(&group_quotas.core_detail);

  group_quotas.enabled = 1;

  group_quotas.time_of_minit = _phpgettimeofday();

  group_begin(&group_quotas.prev_entry, "{main}");
}
#endif

static void stop_watch(TSRMLS_D) {
  
  while (group_quotas.prev_entry) {
    group_end(&group_quotas.prev_entry);
  }

  if (group_quotas.enabled) {
    zend_execute_ex  = _zend_execute_ex;
    zend_execute_internal = _zend_execute_internal;
    group_quotas.enabled = 0;
  }
}

#if PHP_VERSION_ID < 70000
static void clear_quotas() {
  if (group_quotas.core_detail) {
      zval_dtor(group_quotas.core_detail);
      FREE_ZVAL(group_quotas.core_detail);
      group_quotas.core_detail = NULL;
  }

  //group_quotas.prev_entry = NULL;
  group_quotas.entry_free_list = NULL;
}
#else
static void clear_quotas() {
  ZVAL_NULL(&group_quotas.core_detail);
  //group_quotas.prev_entry = NULL;
  group_quotas.entry_free_list = NULL;
}
#endif

static void free_the_free_list() {
  group_entry *p = group_quotas.entry_free_list;
  group_entry *cur;

  while (p) {
    cur = p;
    p = p->prev_entry;
    free(cur);
  }
}

PHP_INI_BEGIN()
    PHP_INI_ENTRY("group_apm.enabled",      "1", PHP_INI_ALL, NULL)
PHP_INI_END()

#if PHP_VERSION_ID < 70000
PHP_FUNCTION(group_apm)
{ 
  if (!group_quotas.enabled) {
    array_init(return_value);
    return;
  }

  stop_watch(TSRMLS_C);

  zval *detail;

  detail = group_quotas.core_detail;
  zval_copy_ctor(detail);

  array_init(return_value);
  add_assoc_zval(return_value, "func_res", detail);

  group_quotas.time_of_mshutdown = _phpgettimeofday();
  double total_time = group_quotas.time_of_mshutdown - group_quotas.time_of_minit;
  add_assoc_double(return_value, "total_time", total_time);
}
#else
PHP_FUNCTION(group_apm)
{ 
  if (!group_quotas.enabled) {
    array_init(return_value);
    return;
  }

  stop_watch(TSRMLS_C);

  zval detail;

  detail = group_quotas.core_detail;
  zval_copy_ctor(&detail);

  array_init(return_value);
  add_assoc_zval(return_value, "func_res", &detail);

  group_quotas.time_of_mshutdown = _phpgettimeofday();
  double total_time = group_quotas.time_of_mshutdown - group_quotas.time_of_minit;
  add_assoc_double(return_value, "total_time", total_time);
}
#endif

PHP_MINIT_FUNCTION(group_apm)
{   
  REGISTER_INI_ENTRIES();

  //begin_watch();

  //group_quotas.cpu_num = sysconf(_SC_NPROCESSORS_CONF);

  return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(group_apm)
{ 
  free_the_free_list();

  UNREGISTER_INI_ENTRIES();

  return SUCCESS;
}

PHP_RINIT_FUNCTION(group_apm)
{
#if PHP_VERSION_ID < 70000
  group_quotas.core_detail = NULL;
#else
  ZVAL_NULL(&group_quotas.core_detail);
#endif
  group_quotas.entry_free_list = NULL;
  group_quotas.id = 0;

  long enabled = INI_INT("group_apm.enabled");
  if (enabled == 1) {
    begin_watch(TSRMLS_C);
  }
  
  return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(group_apm)
{
  stop_watch(TSRMLS_C);
  clear_quotas();

  return SUCCESS;
}

PHP_MINFO_FUNCTION(group_apm)
{ 
  php_info_print_table_start();
  php_info_print_table_header(2, "group_apm", GROUPAPM_VERSION);
  php_info_print_table_header(2, "group_apm support", "enabled");
  php_info_print_table_end();

  DISPLAY_INI_ENTRIES();
}

const zend_function_entry group_apm_functions[] = {
  PHP_FE(group_apm, NULL)
  PHP_FE_END
};

zend_module_entry group_apm_module_entry = {
  STANDARD_MODULE_HEADER,
  "group_apm",
  group_apm_functions,
  PHP_MINIT(group_apm),
  PHP_MSHUTDOWN(group_apm),
  PHP_RINIT(group_apm),
  PHP_RSHUTDOWN(group_apm),
  PHP_MINFO(group_apm),
  PHP_GROUP_APM_VERSION,
  STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_GROUP_APM
ZEND_GET_MODULE(group_apm)
#endif
