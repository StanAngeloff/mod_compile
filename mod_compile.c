#include "apr.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "apr_hash.h"
#include "apr_buckets.h"
#define APR_WANT_STRFUNC
#include "apr_want.h"

#include "httpd.h"
#include "http_log.h"
#include "http_config.h"

#include "util_filter.h"

module AP_MODULE_DECLARE_DATA compile_module;

typedef enum {
  COMPILE_FLAG_UNSET    = 0x00,
  COMPILE_FLAG_ENABLED  = 0x01,
  COMPILE_FLAG_DISABLED = 0x02
} compile_flag_t;

typedef struct compile_config_t {
  compile_flag_t      enabled;
  compile_flag_t      use_path_info;
  apr_hash_t         *extension_commands;
  apr_array_header_t *removed_commands;
} compile_config_t;

typedef struct compile_extension_config_t {
  char *command_line;
} compile_extension_config_t;

typedef struct compile_attribute_config_t {
  char *name;
} compile_attribute_config_t;

static compile_config_t *compile_create_config(apr_pool_t *pool) {
  compile_config_t *config;
  config = (compile_config_t*) apr_pcalloc(pool, sizeof (compile_config_t));
  config->enabled       = COMPILE_FLAG_UNSET;
  config->use_path_info = COMPILE_FLAG_UNSET;
  config->extension_commands = NULL;
  config->removed_commands   = NULL;
  return config;
}

static void *compile_create_directory_config(apr_pool_t *pool, char *path) {
  return (void*) compile_create_config(pool);
}

static void *compile_create_server_config(apr_pool_t *pool, server_rec *server) {
  return (void*) compile_create_config(pool);
}

#define COMPILE_MERGE_FLAG(_key) \
  config->_key = (overrides->_key != COMPILE_FLAG_UNSET ? overrides->_key : base->_key)

static void *compile_overlay_extension_commands(apr_pool_t *pool, const void *key, apr_ssize_t key_length, const void *config1, const void *config2, const void *ununsed) {
  compile_extension_config_t       *merged   = apr_palloc(pool, sizeof (compile_extension_config_t));
  const compile_extension_config_t *override = (const compile_extension_config_t*) config1;
  const compile_extension_config_t *base     = (const compile_extension_config_t*) config2;
  memcpy(merged, base, sizeof (compile_extension_config_t));
  if (override->command_line) {
    merged->command_line = override->command_line;
  }
  return merged;
}

static void compile_remove_extension_commands(apr_pool_t *pool, apr_array_header_t *list, apr_hash_t *commands) {
  compile_attribute_config_t *attribute = (compile_attribute_config_t*) list->elts;
  int i;
  for (i = 0; i < list->nelts; i ++) {
    compile_extension_config_t *extension_config = apr_hash_get(commands, attribute[i].name, APR_HASH_KEY_STRING);
    if (extension_config && extension_config->command_line) {
      compile_extension_config_t *extension_copy = extension_config;
      extension_config = (compile_extension_config_t*) apr_palloc(pool, sizeof (*extension_config));
      apr_hash_set(commands, attribute[i].name, APR_HASH_KEY_STRING, extension_config);
      memcpy(extension_config, extension_copy, sizeof (*extension_config));
      extension_config->command_line = NULL;
    }
  }
}

static void *compile_merge_config(apr_pool_t *pool, void *config1, void *config2) {
  compile_config_t *base      = (compile_config_t*) config1;
  compile_config_t *overrides = (compile_config_t*) config2;
  compile_config_t *config;
  config = (compile_config_t*) apr_pcalloc(pool, sizeof (compile_config_t));
  COMPILE_MERGE_FLAG(enabled);
  COMPILE_MERGE_FLAG(use_path_info);
  if (base->extension_commands && overrides->extension_commands) {
    config->extension_commands = apr_hash_merge(pool, overrides->extension_commands, base->extension_commands, compile_overlay_extension_commands, NULL);
  } else {
    if (base->extension_commands == NULL) {
      config->extension_commands = overrides->extension_commands;
    } else {
      config->extension_commands = base->extension_commands;
    }
    if (config->extension_commands && overrides->removed_commands) {
      config->extension_commands = apr_hash_copy(pool, config->extension_commands);
    }
  }
  if (config->extension_commands) {
    if (overrides->removed_commands) {
      compile_remove_extension_commands(pool, overrides->removed_commands, config->extension_commands);
    }
  }
  config->removed_commands = NULL;
  return (void*) config;
}

#undef COMPILE_MERGE_FLAG

#define COMPILE_FIND_CONFIG \
  compile_config_t *config = (compile_config_t*) overrides; \
  if (cmd->path == NULL) { \
    config = (compile_config_t*) ap_get_module_config(cmd->server->module_config, &compile_module); \
  } \
  if ( ! config) { \
    return "(mod_compile) Cannot determine the configuration object."; \
  }

#define COMPILE_SET_FLAG(_name, _key) \
  if ( ! strcasecmp(cmd->cmd->name, _name)) { \
    config->_key = (flag ? COMPILE_FLAG_ENABLED : COMPILE_FLAG_DISABLED); \
  }

static const char *compile_command_flag(cmd_parms *cmd, void *overrides, int flag) {
  COMPILE_FIND_CONFIG;
#ifdef _DEBUG
  ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, cmd->server, "compile_command_flag('%s', %d)", cmd->cmd->name, flag);
#endif
       COMPILE_SET_FLAG("compile", enabled)
  else COMPILE_SET_FLAG("modcompileusepathinfo", use_path_info)
  else {
    return (char*) apr_psprintf(cmd->pool, "(mod_compile) '%s %s' is not a valid command in this context.", cmd->cmd->name, (flag ? "On": "Off"));
  }
  return NULL;
}

#undef COMPILE_SET_FLAG

static const char *compile_command_add_extension(cmd_parms *cmd, void *overrides, const char *value, const char* extension) {
  if ( ! ap_strstr(value, "%s")) {
    return (char*) apr_psprintf(cmd->pool, "(mod_compile) 'AddCompileCommand %s' does not contain a '%%s'.", value);
  }
  COMPILE_FIND_CONFIG;
#ifdef _DEBUG
  ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, cmd->server, "compile_command_add_extension('%s', '%s')", value, extension);
#endif
  compile_extension_config_t *extension_config;
  char *value_normalized     = apr_pstrdup(cmd->pool,      value);
  char *extension_normalized = apr_pstrdup(cmd->temp_pool, extension);
  ap_str_tolower(value_normalized);
  ap_str_tolower(extension_normalized);
  if (*extension_normalized == '.') {
     ++ extension_normalized;
  }
  if ( ! config->extension_commands) {
    config->extension_commands = apr_hash_make(cmd->pool);
    extension_config = NULL;
  } else {
    extension_config = (compile_extension_config_t*) apr_hash_get(config->extension_commands, extension_normalized, APR_HASH_KEY_STRING);
  }
  if ( ! extension_config) {
     extension_config     = apr_pcalloc(cmd->pool, sizeof (compile_extension_config_t));
     extension_normalized = apr_pstrdup(cmd->pool, extension_normalized);
     apr_hash_set(config->extension_commands, extension_normalized, APR_HASH_KEY_STRING, extension_config);
  }
  extension_config->command_line = value_normalized;
  return NULL;
}

static const char *compile_command_remove_extension(cmd_parms *cmd, void *overrides, const char *extension) {
  COMPILE_FIND_CONFIG;
  compile_attribute_config_t *attribute;
  if (*extension == '.') {
    ++ extension;
  }
  if ( ! config->removed_commands) {
    config->removed_commands = apr_array_make(cmd->pool, 4, sizeof (*attribute));
  }
  attribute = (compile_attribute_config_t*) apr_array_push(config->removed_commands);
  attribute->name = apr_pstrdup(cmd->pool, extension);
  ap_str_tolower(attribute->name);
  return NULL;
}

#undef COMPILE_FIND_CONFIG

static const command_rec compile_command_table[] = {
  AP_INIT_FLAG(
    "Compile",
    compile_command_flag,
    NULL,
    OR_FILEINFO,
    "On|Off - Enable/Disable mod_compile (default: Off)"
  ),
  AP_INIT_FLAG(
    "ModCompileUsePathInfo",
    compile_command_flag,
    NULL,
    OR_FILEINFO,
    "On|Off - Enable/Disable the use of PATH_INFO for compile command checking (default: Off)"
  ),
  AP_INIT_ITERATE2(
    "AddCompileCommand",
    compile_command_add_extension,
    NULL,
    OR_FILEINFO,
    "A compile command to execute (e.g., '/usr/bin/coffee -cp %s'), followed by one or more file extensions"
  ),
  AP_INIT_ITERATE(
    "RemoveCompileCommand",
    compile_command_remove_extension,
    NULL,
    OR_FILEINFO,
    "One or more file extensions"
  )
};

static apr_status_t ap_compile_output_filter(ap_filter_t *filter, apr_bucket_brigade *input_brigade) {
  request_rec *request = filter->r;
  if ( ! request->filename) {
    return ap_pass_brigade(filter->next, input_brigade);
  }
  const char       *resource_name;
  compile_config_t *directory_config = (compile_config_t*) ap_get_module_config(request->per_dir_config,        &compile_module);
  compile_config_t *server_config    = (compile_config_t*) ap_get_module_config(request->server->module_config, &compile_module);
  compile_config_t *config           = compile_merge_config(request->pool, server_config, directory_config);
  if (config->use_path_info) {
    resource_name = apr_pstrcat(request->pool, request->filename, request->path_info, NULL);
  } else {
    resource_name = request->filename;
  }
  const char *filename;
        char *extension;
  filename = ap_strrchr_c(resource_name, '/');
  if (filename == NULL) {
    filename = resource_name;
  } else {
    ++ filename;
  }
  extension = ap_getword(request->pool, &filename, '.');
  while (*filename && (extension = ap_getword(request->pool, &filename, '.'))) {
    if (*extension == '\0') {
      continue;
    }
    ap_str_tolower(extension);
    if (config->extension_commands != NULL) {
      const compile_extension_config_t *extension_config = NULL;
      extension_config = (compile_extension_config_t*) apr_hash_get(config->extension_commands, extension, APR_HASH_KEY_STRING);
      if (extension_config != NULL && extension_config->command_line) {
#ifdef _DEBUG
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, request->server, "ap_compile_output_filter('%s')", apr_psprintf(request->pool, extension_config->command_line, resource_name));
#endif
        // TODO: http://svn.apache.org/repos/asf/httpd/httpd/tags/2.2.6/modules/experimental/mod_case_filter.c
        // Collect buckets, save to disk, run command line and tail-insert result
        ap_set_content_type(request, "text/html;charset=utf-8");
        break;
      }
    }
  }
  return ap_pass_brigade(filter->next, input_brigade);
}

static void ap_compile_add_output_filter(request_rec *request) {
  compile_flag_t enabled = ((compile_config_t*) ap_get_module_config(request->per_dir_config, &compile_module))->enabled;
  if (enabled == COMPILE_FLAG_UNSET) {
    enabled = ((compile_config_t*) ap_get_module_config(request->server->module_config, &compile_module))->enabled;
  }
  if (enabled == COMPILE_FLAG_ENABLED) {
    ap_add_output_filter("COMPILE", NULL, request, request->connection);
  }
}

static void compile_register_hooks(apr_pool_t *pool) {
  ap_hook_insert_filter(ap_compile_add_output_filter, NULL, NULL, APR_HOOK_MIDDLE);
  ap_register_output_filter("COMPILE", ap_compile_output_filter, NULL, AP_FTYPE_RESOURCE);
}

module AP_MODULE_DECLARE_DATA compile_module = {
  STANDARD20_MODULE_STUFF,
  compile_create_directory_config,
  compile_merge_config,
  compile_create_server_config,
  compile_merge_config,
  compile_command_table,
  compile_register_hooks
};

// TODO: Short-term:
// Ensure mod_compile plays nice with:
//   * mod_expires
//   * mod_mime
//   * mod_rewrite

// TODO: Mid-term:
// Inject Last-Modified, ETag, Cache-Control, etc. headers

// TODO: Long-term:
// Implement caching providers - memory, file, memcache(d)
