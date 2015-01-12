/*
 * balde: A microframework for C based on GLib and bad intentions.
 * Copyright (C) 2013-2015 Rafael G. Martins <rafael@rafaelmartins.eng.br>
 *
 * This program can be distributed under the terms of the LGPL-2 License.
 * See the file COPYING.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <glib.h>
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include "balde.h"
#include "balde-private.h"
#include "app.h"
#include "cgi.h"
#include "exceptions.h"
#include "resources.h"
#include "routing.h"
#include "wrappers.h"

#ifdef BUILD_HTTP
#include "httpd.h"
#endif

#ifdef BUILD_FASTCGI
#include <fcgiapp.h>
#include "fcgi.h"
#endif


static GLogLevelFlags
balde_get_log_level_flag_from_string(const gchar *level)
{
    if (level == NULL)
        return G_LOG_LEVEL_MESSAGE;

    gchar *level_str = g_ascii_strup(level, -1);

    GLogLevelFlags level_flag = G_LOG_LEVEL_MESSAGE;
    if (g_strcmp0(level_str, "CRITICAL") == 0)
        level_flag = G_LOG_LEVEL_CRITICAL;
    else if (g_strcmp0(level_str, "WARNING") == 0)
        level_flag = G_LOG_LEVEL_WARNING;
    else if (g_strcmp0(level_str, "MESSAGE") == 0)
        level_flag = G_LOG_LEVEL_MESSAGE;
    else if (g_strcmp0(level_str, "INFO") == 0)
        level_flag = G_LOG_LEVEL_INFO;
    else if (g_strcmp0(level_str, "DEBUG") == 0)
        level_flag = G_LOG_LEVEL_DEBUG;
    else
        g_printerr("ERROR: Invalid log level, defaulting to MESSAGE ...\n");

    g_free(level_str);
    return level_flag;
}


static void
balde_log_handler(const gchar *log_domain, GLogLevelFlags log_level,
    const gchar *message, gpointer user_data)
{
    GLogLevelFlags wanted_log_level = GPOINTER_TO_INT(user_data);
    if (log_level <= wanted_log_level) {
        const gchar *level_str;
        switch (log_level & G_LOG_LEVEL_MASK) {
            case G_LOG_LEVEL_ERROR:
                return;  // INVALID
            case G_LOG_LEVEL_CRITICAL:
                level_str = "CRITICAL";
                break;
            case G_LOG_LEVEL_WARNING:
                level_str = "WARNING";
                break;
            case G_LOG_LEVEL_MESSAGE:
                level_str = "MESSAGE";
                break;
            case G_LOG_LEVEL_INFO:
                level_str = "INFO";
                break;
            case G_LOG_LEVEL_DEBUG:
                level_str = "DEBUG";
                break;
        }
        fprintf(stderr, "%s: %s\n", level_str, message);
    }
}


BALDE_API balde_app_t*
balde_app_init(void)
{
    balde_app_t *app = g_new(balde_app_t, 1);
    app->priv = g_new(struct _balde_app_private_t, 1);
    app->priv->views = NULL;
    app->priv->before_requests = NULL;
    app->priv->static_resources = NULL;
    app->priv->user_data = NULL;
    app->priv->user_data_destroy_func = NULL;
    app->priv->config = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    app->copy = FALSE;
    app->error = NULL;
    balde_app_add_url_rule(app, "static", "/static/<path:file>", BALDE_HTTP_GET,
        balde_resource_view);
    return app;
}


balde_app_t*
balde_app_copy(balde_app_t *app)
{
    balde_app_t *copy = g_new(balde_app_t, 1);
    copy->error = NULL;
    copy->copy = TRUE;
    copy->priv = app->priv;
    return copy;
}


G_LOCK_DEFINE_STATIC(config);

BALDE_API void
balde_app_set_config(balde_app_t *app, const gchar *name, const gchar *value)
{
    BALDE_APP_READ_ONLY(app);
    G_LOCK(config);
    g_hash_table_replace(app->priv->config, g_utf8_strdown(name, -1), g_strdup(value));
    G_UNLOCK(config);
}


BALDE_API void
balde_app_set_config_from_envvar(balde_app_t *app, const gchar *name,
    const gchar *env_name, gboolean silent)
{
    BALDE_APP_READ_ONLY(app);
    const gchar *value = g_getenv(env_name);
    if (value == NULL && !silent) {
        gchar *msg = g_strdup_printf("%s environment variable must be set",
            env_name);
        balde_abort_set_error_with_description(app, 500, msg);
        g_free(msg);
        return;
    }
    balde_app_set_config(app, name, value);
}


BALDE_API const gchar*
balde_app_get_config(balde_app_t *app, const gchar *name)
{
    gchar *tmp = g_utf8_strdown(name, -1);
    const gchar *rv = g_hash_table_lookup(app->priv->config, tmp);
    g_free(tmp);
    return rv;
}


BALDE_API void
balde_app_set_user_data(balde_app_t *app, gpointer user_data)
{
    BALDE_APP_READ_ONLY(app);

    // when setting, if we have a destroy function, try to use it.
    balde_app_free_user_data(app);

    app->priv->user_data = user_data;
}


BALDE_API gpointer
balde_app_get_user_data(balde_app_t *app)
{
    return app->priv->user_data;
}


BALDE_API void
balde_app_set_user_data_destroy_func(balde_app_t *app, GDestroyNotify destroy_func)
{
    BALDE_APP_READ_ONLY(app);
    app->priv->user_data_destroy_func = destroy_func;
}


BALDE_API void
balde_app_free_user_data(balde_app_t *app)
{
    if (app->priv->user_data_destroy_func != NULL && app->priv->user_data != NULL) {
        app->priv->user_data_destroy_func(app->priv->user_data);
        app->priv->user_data = NULL;
    }
}


void
balde_app_free_views(balde_view_t *view)
{
    balde_free_url_rule_match(view->url_rule->match);
    g_free(view->url_rule);
    g_free(view);
}


BALDE_API void
balde_app_free(balde_app_t *app)
{
    if (app == NULL)
        return;
    if (!app->copy) {
        g_slist_free_full(app->priv->views, (GDestroyNotify) balde_app_free_views);
        g_slist_free(app->priv->before_requests);
        g_slist_free_full(app->priv->static_resources, (GDestroyNotify) balde_resource_free);
        g_hash_table_destroy(app->priv->config);
        balde_app_free_user_data(app);
        g_free(app->priv);
    }
    g_clear_error(&app->error);
    g_free(app);
}


G_LOCK_DEFINE_STATIC(views);

BALDE_API void
balde_app_add_url_rule(balde_app_t *app, const gchar *endpoint, const gchar *rule,
    const balde_http_method_t method, balde_view_func_t view_func)
{
    BALDE_APP_READ_ONLY(app);
    GError *tmp_error = NULL;
    balde_view_t *view = g_new(balde_view_t, 1);
    view->url_rule = g_new(balde_url_rule_t, 1);
    view->url_rule->endpoint = endpoint;
    view->url_rule->rule = rule;
    view->url_rule->match = balde_parse_url_rule(view->url_rule->rule, &tmp_error);
    if (tmp_error != NULL) {
        g_propagate_error(&(app->error), tmp_error);
        balde_app_free_views(view);
        return;
    }
    view->url_rule->method = method | BALDE_HTTP_OPTIONS;
    if (view->url_rule->method & BALDE_HTTP_GET)
        view->url_rule->method |= BALDE_HTTP_HEAD;
    view->view_func = view_func;
    G_LOCK(views);
    app->priv->views = g_slist_append(app->priv->views, view);
    G_UNLOCK(views);
}


G_LOCK_DEFINE_STATIC(before_requests);

BALDE_API void
balde_app_add_before_request(balde_app_t *app, balde_before_request_func_t hook_func)
{
    BALDE_APP_READ_ONLY(app);
    G_LOCK(before_requests);
    app->priv->before_requests = g_slist_append(app->priv->before_requests, hook_func);
    G_UNLOCK(before_requests);
}


balde_view_t*
balde_app_get_view_from_endpoint(balde_app_t *app, const gchar *endpoint)
{
    for (GSList *tmp = app->priv->views; tmp != NULL; tmp = g_slist_next(tmp)) {
        balde_view_t *view = tmp->data;
        if (0 == g_strcmp0(view->url_rule->endpoint, endpoint))
            return view;
    }
    return NULL;
}


BALDE_API gchar*
balde_app_url_for(balde_app_t *app, balde_request_t *request,
    const gchar *endpoint, gboolean external, ...)
{
    va_list params;
    va_start(params, external);
    gchar *rv = balde_app_url_forv(app, request, endpoint, params);
    va_end(params);
    return rv;
}


gchar*
balde_app_url_forv(balde_app_t *app, balde_request_t *request,
    const gchar *endpoint, va_list params)
{
    balde_view_t *view = balde_app_get_view_from_endpoint(app, endpoint);
    if (view == NULL)
        return NULL;
    const gchar *script_name = request->script_name;
    GString *p = g_string_new(script_name == NULL ? "" : script_name);
    for (guint i = 0; view->url_rule->match->pieces[i] != NULL; i++) {
        g_string_append(p, view->url_rule->match->pieces[i]);
        if (view->url_rule->match->pieces[i + 1] != NULL)
            g_string_append(p, va_arg(params, const gchar*));
    }
    gchar *tmp = g_string_free(p, FALSE);
    gchar *rv = g_uri_escape_string(tmp, "/:", TRUE);
    g_free(tmp);
    return rv;
}


/**
 * \example hello.c
 *
 * A hello world!
 */

static gboolean help = FALSE;
static gboolean version = FALSE;
static gchar *log_level = NULL;

#ifdef BUILD_HTTP
static gboolean runserver = FALSE;
#endif

#ifdef BUILD_FASTCGI
static gboolean runfcgi = FALSE;
#endif

static GOptionEntry entries[] =
{
    {"help", 'h', 0, G_OPTION_ARG_NONE, &help,
        "Show help options", NULL},
    {"version", 'v', 0, G_OPTION_ARG_NONE, &version,
        "Show balde's version number and exit.", NULL},
    {"log-level", 'l', 0, G_OPTION_ARG_STRING, &log_level,
        "Logging level (CRITICAL, WARNING, MESSAGE, INFO, DEBUG). "
        "(default: MESSAGE)", "LEVEL"},

#ifdef BUILD_HTTP
    {"runserver", 's', 0, G_OPTION_ARG_NONE, &runserver,
        "Run embedded HTTP server. NOT production ready!", NULL},
#endif

#ifdef BUILD_FASTCGI
    {"runfcgi", 'f', 0, G_OPTION_ARG_NONE, &runfcgi,
        "Listen to FastCGI socket.", NULL},
#endif

    {NULL}
};


#ifdef BUILD_HTTP
static gchar *http_host = NULL;
static gint16 http_port = 8080;
static guint64 http_max_threads = 10;

static GOptionEntry entries_http[] =
{
    {"http-host", 0, 0, G_OPTION_ARG_STRING, &http_host,
        "Embedded HTTP server host. (default: 127.0.0.1)", "HOST"},
    {"http-port", 0, 0, G_OPTION_ARG_INT, &http_port,
        "Embedded HTTP server port. (default: 8080)", "PORT"},
    {"http-max-threads", 0, 0, G_OPTION_ARG_INT, &http_max_threads,
        "Embedded HTTP server max threads. (default: 10)", "THREADS"},
    {NULL}
};
#endif

#ifdef BUILD_FASTCGI
static gchar *fcgi_host = NULL;
static gint16 fcgi_port = 1026;
static gchar *fcgi_socket = NULL;
static gint fcgi_socket_mode = -1;
static guint64 fcgi_max_threads = 1;
static gint fcgi_backlog = 1024;


static gboolean
balde_socket_mode_func(const gchar *option_name, const gchar *value,
    gpointer data, GError **error)
{
    if (value != NULL)
        fcgi_socket_mode = strtol(value, NULL, 8);
    return TRUE;
}


static GOptionEntry entries_fcgi[] =
{
    {"fcgi-host", 0, 0, G_OPTION_ARG_STRING, &fcgi_host,
        "FastCGI host, conflicts with UNIX socket. (default: 127.0.0.1)", "HOST"},
    {"fcgi-port", 0, 0, G_OPTION_ARG_INT, &fcgi_port,
        "FastCGI port, conflicts with UNIX socket. (default: 1026)", "PORT"},
    {"fcgi-socket", 0,0, G_OPTION_ARG_STRING, &fcgi_socket,
        "FastCGI UNIX socket path, conflicts with host and port. (default: not set)",
        "SOCKET"},
    {"fcgi-socket-mode", 0, 0, G_OPTION_ARG_CALLBACK, balde_socket_mode_func,
        "FastCGI UNIX socket mode, octal integer. (default: umask)", "MODE"},
    {"fcgi-max-threads", 0, 0, G_OPTION_ARG_INT, &fcgi_max_threads,
        "FastCGI max threads. (default: 1)", "THREADS"},
    {"fcgi-backlog", 0, 0, G_OPTION_ARG_INT, &fcgi_backlog,
        "FastCGI socket backlog. (default: 1024)", "BACKLOG"},
    {NULL}
};
#endif


BALDE_API void
balde_app_run(balde_app_t *app, gint argc, gchar **argv)
{
    setlocale(LC_ALL, "");
    GError *err = NULL;
    GOptionContext *context = g_option_context_new("- a balde application ;-)");
    g_option_context_add_main_entries(context, entries, NULL);

#ifdef BUILD_HTTP
    GOptionGroup *http_group = g_option_group_new("http", "HTTP Options:",
        "Show HTTP help options", NULL, NULL);
    g_option_group_add_entries(http_group, entries_http);
    g_option_context_add_group(context, http_group);
#endif

#ifdef BUILD_FASTCGI
    GOptionGroup *fcgi_group = g_option_group_new("fastcgi", "FastCGI Options:",
        "Show FastCGI help options", NULL, NULL);
    g_option_group_add_entries(fcgi_group, entries_fcgi);
    g_option_context_add_group(context, fcgi_group);
#endif

    g_option_context_set_help_enabled(context, FALSE);

    if (!g_option_context_parse(context, &argc, &argv, &err)) {
        g_printerr("Option parsing failed: %s\n", err->message);
        exit(1);
    }

    g_log_set_handler(BALDE_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL |
        G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO |
        G_LOG_LEVEL_DEBUG, balde_log_handler,
        GINT_TO_POINTER(balde_get_log_level_flag_from_string(log_level)));

#ifdef BUILD_HTTP
#ifdef BUILD_FASTCGI
    if (runserver && runfcgi) {
        g_printerr("ERROR: --runserver conflicts with --runfcgi\n");
        goto clean;
    }
    if (http_host != NULL && (fcgi_host != NULL || fcgi_socket || fcgi_socket_mode > 0)) {
        g_printerr("ERROR: most --host-* arguments are incompatible with most "
            "--fcgi-* arguments\n");
        goto clean;
    }
#endif
#endif

#ifdef BUILD_HTTP
    if (http_host != NULL)
        runserver = TRUE;
#endif

#ifdef BUILD_FASTCGI
    if (fcgi_host != NULL || fcgi_socket || fcgi_socket_mode > 0)
        runfcgi = TRUE;
#endif

    if (help) {
        gchar *help_str = g_option_context_get_help(context, FALSE, NULL);
        g_print("%s", help_str);
        g_free(help_str);
    }

    else if (version)
        g_printerr("%s\n", PACKAGE_STRING);

#ifdef BUILD_HTTP
    else if (runserver)
        balde_httpd_run(app, http_host, http_port, http_max_threads);
#endif

#ifdef BUILD_FASTCGI
    else if (runfcgi || !FCGX_IsCGI()) {
        if (fcgi_socket != NULL && fcgi_host != NULL) {
            g_printerr("ERROR: --fcgi-socket conflicts with --fcgi-host\n");
            goto clean;
        }
        const gchar *threads_str = g_getenv("BALDE_FASTCGI_THREADS");
        guint64 threads = fcgi_max_threads;
        if (threads_str != NULL && threads_str[0] != '\0')
            threads = g_ascii_strtoull(threads_str, NULL, 10);
        balde_fcgi_run(app, fcgi_host, fcgi_port, fcgi_socket, fcgi_socket_mode,
            threads, fcgi_backlog, runfcgi);
    }
#endif

    else if (g_getenv("REQUEST_METHOD") != NULL)
        balde_cgi_run(app);
    else {
        gchar *help_str = g_option_context_get_help(context, FALSE, NULL);
        g_printerr("%s", help_str);
        g_free(help_str);
    }

clean:
    g_option_context_free(context);

#ifdef BUILD_HTTP
    g_free(http_host);
#endif

#ifdef BUILD_FASTCGI
    g_free(fcgi_host);
    g_free(fcgi_socket);
#endif

    g_free(log_level);
}


GString*
balde_app_main_loop(balde_app_t *app, balde_request_env_t *env,
    balde_response_render_t render, balde_http_exception_code_t *status_code)
{
    balde_request_t *request;
    balde_response_t *response;
    balde_response_t *error_response;
    gchar *endpoint;
    gboolean with_body = TRUE;
    GString *rv = NULL;

    // render startup error, if any
    if (app->error != NULL) {
        error_response = balde_make_response_from_exception(app->error);
        rv = render(error_response, with_body);
        if (status_code != NULL)
            *status_code = error_response->status_code;
        balde_response_free(error_response);
        return rv;
    }

    request = balde_make_request(app, env);

    for (GSList *tmp = app->priv->before_requests; tmp != NULL; tmp = g_slist_next(tmp)) {
        balde_before_request_func_t hook_func = tmp->data;
        hook_func(app, request);
    }

    balde_app_t *app_copy = balde_app_copy(app);

    with_body = ! (request->method & BALDE_HTTP_HEAD);

    // get the view
    endpoint = balde_dispatch_from_path(app_copy->priv->views, request->path,
        &(request->priv->view_args));
    if (endpoint == NULL) {  // no view found! :(
        balde_abort_set_error(app_copy, 404);
    }
    else {
        // validate http method
        balde_view_t *view = balde_app_get_view_from_endpoint(app_copy, endpoint);
        if (request->method & view->url_rule->method) {
            // answer OPTIONS automatically
            if (request->method == BALDE_HTTP_OPTIONS) {
                response = balde_make_response("");
                gchar *allow = balde_list_allowed_methods(view->url_rule->method);
                balde_response_set_header(response, "Allow", allow);
                g_free(allow);
            }
            // run the view
            else {
                response = view->view_func(app_copy, request);
            }
        }
        // method not allowed
        else {
            balde_abort_set_error(app_copy, 405);
        }
        g_free(endpoint);
    }

    balde_request_free(request);

    if (app_copy->error != NULL) {
        error_response = balde_make_response_from_exception(app_copy->error);
        rv = render(error_response, with_body);
        if (status_code != NULL)
            *status_code = error_response->status_code;
        balde_response_free(error_response);
        balde_app_free(app_copy);
        return rv;
    }

    rv = render(response, with_body);
    if (status_code != NULL)
        *status_code = response->status_code;
    balde_response_free(response);
    balde_app_free(app_copy);

    return rv;
}