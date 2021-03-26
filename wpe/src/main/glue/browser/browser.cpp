#include <algorithm>

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <gobject/gvaluecollector.h>

#include <wpe-android/view-backend-exportable.h>

#include <jni.h>

#include "browser.h"
#include "logging.h"

static GMutex initMutex;
static GCond initCond;

static GMainContext *uiProcessThreadContext = NULL;
static GMainLoop *uiProcessThreadLoop;

static wpe_android_view_backend_exportable *s_viewBackendExportable;

// FIXME: tabs support. Use:
//        static std::unordered_map<WebKitWebView*, std::unique_ptr<PageEventObserver>> pageObservers;
static std::unique_ptr<PageEventObserver> pageObserver;

static void ui_process_thread_entry() {
    pipe_stdout_to_logcat();
    enable_gst_debug();

    g_mutex_lock(&initMutex);
    ALOGV("ui_process_thread_entry -- entered, g_main_context_default() %p",
          g_main_context_default());

    uiProcessThreadLoop = g_main_loop_new(uiProcessThreadContext, FALSE);

    g_cond_signal(&initCond);
    g_mutex_unlock(&initMutex);

    g_main_context_push_thread_default(uiProcessThreadContext);

    webkit_web_context_new();

    ALOGV("ui_process_thread_entry -- running via GMainLoop %p for GMainContext %p",
          uiProcessThreadLoop, uiProcessThreadContext);
    g_main_loop_run(uiProcessThreadLoop);
    ALOGV("ui_process_thread_entry -- quitting");

    g_main_context_pop_thread_default(uiProcessThreadContext);

    g_main_loop_unref(uiProcessThreadLoop);
    uiProcessThreadLoop = NULL;
}

void wpe_browser_glue_init() {
    ALOGV("wpe_browser_glue_init() tid %d", gettid());

    if (!uiProcessThreadContext) {
        uiProcessThreadContext = g_main_context_new();
    }

    ui_process_thread_entry();
}

void wpe_browser_glue_deinit() {
    ALOGV("wpe_browser_glue_deinit");
    g_main_loop_quit(uiProcessThreadLoop);
}

static void onLoadChanged(WebKitWebView*, WebKitLoadEvent loadEvent, gpointer) {
    pageObserver->onLoadChanged(loadEvent);
}

static void onLoadProgressChanged(WebKitWebView* webView, GParamSpec*, gpointer) {
    gdouble progress = webkit_web_view_get_estimated_load_progress(webView);
    pageObserver->onLoadProgress(progress);
}

typedef struct {
    GMainLoop *mainLoop;
    GMainContext *mainContext;
    int width;
    int height;
    WebKitWebView *webView;
    std::function<void (long)> callback;
    std::unique_ptr<PageEventObserver> observer;
} WebViewInitData;

void wpe_browser_glue_new_web_view(int width, int height,
                                   std::unique_ptr<PageEventObserver> observer,
                                   std::function<void (long)> callback) {
    ALOGV("wpe_browser_glue_new_web_view (%d, %d) tid %d", width, height, gettid());

    GMainLoop *resultLoop = g_main_loop_new(NULL, FALSE);
    auto *data = new WebViewInitData { resultLoop, g_main_context_default(), width, height, NULL, callback, std::move(observer) };

    g_main_context_invoke_full(uiProcessThreadContext, G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        g_assert(g_main_context_is_owner(uiProcessThreadContext));
        ALOGV("Creating a new WebkitWebView tid %d", gettid());
        auto *initData = reinterpret_cast<WebViewInitData*>(data);
        struct wpe_view_backend *wpeBackend;
        {
            s_viewBackendExportable = wpe_android_view_backend_exportable_create(std::max(0, initData->width),
                                                                                 std::max(0, initData->height));
            wpeBackend = wpe_android_view_backend_exportable_get_view_backend(s_viewBackendExportable);
        }

        auto *viewBackend = webkit_web_view_backend_new(wpeBackend, NULL, NULL);

        auto *webView = webkit_web_view_new(viewBackend);

        // FIXME: This needs to be disconnected when the webview is closed. Not a big deal until we
        //        want to support multiple tabs.
        g_signal_connect(webView, "load-changed", G_CALLBACK(onLoadChanged), NULL);
        g_signal_connect(webView, "notify::estimated-load-progress", G_CALLBACK(onLoadProgressChanged), NULL);

        ALOGV("Created WebKitWebView %p", webView);

        initData->webView = webView;

        g_main_context_invoke_full(initData->mainContext, G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
            auto *initData = reinterpret_cast<WebViewInitData*>(data);
            initData->callback((long)initData->webView);
            g_main_loop_quit(initData->mainLoop);

            pageObserver = std::move(initData->observer);

            return G_SOURCE_REMOVE;
        }, initData, NULL);

        return G_SOURCE_REMOVE;
    }, data, NULL);
    g_main_loop_run(resultLoop);
    g_main_loop_unref(resultLoop);
}

void wpe_browser_glue_close_web_view(jlong webView) {
    ALOGV("wpe_browser_glue_close_web_view %p", (WebKitWebView*)webView);
    g_main_context_invoke_full(uiProcessThreadContext, G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        auto *view = reinterpret_cast<WebKitWebView*>(data);
        ALOGV("Closing WebkitWebView %p", view);
        webkit_web_view_try_close(view);
        return G_SOURCE_REMOVE;
    }, reinterpret_cast<WebKitWebView*>(webView), NULL);
}

typedef struct {
    WebKitWebView *webView;
    char *url;
} LoadUrlData;

void wpe_browser_glue_load_url(jlong webView, const char *urlData, jsize urlSize) {
    char *url = g_strndup(urlData, urlSize);

    ALOGV("wpe_browser_glue_set_page_url -- URL %s WebKitWebView %p thread %p", url, (WebKitWebView*)webView, uiProcessThreadContext);

    auto *data = new LoadUrlData { (WebKitWebView*)webView, url };
    g_main_context_invoke_full(uiProcessThreadContext, G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        auto *loadUrlData = reinterpret_cast<LoadUrlData*>(data);

        ALOGV("load_url %s", loadUrlData->url);

        webkit_web_view_load_uri(loadUrlData->webView, loadUrlData->url);

        return G_SOURCE_REMOVE;
    }, data, [](gpointer data) -> void {
        auto *loadUrlData = reinterpret_cast<LoadUrlData*>(data);
        g_free(loadUrlData->url);
    });
}

void wpe_browser_glue_go_back(jlong webView) {
    webkit_web_view_go_back((WebKitWebView*)webView);
}

void wpe_browser_glue_go_forward(jlong webView) {
    webkit_web_view_go_forward((WebKitWebView*)webView);
}

void wpe_browser_glue_reload(jlong webView) {
    webkit_web_view_reload((WebKitWebView*)webView);
}

static gboolean frame_complete_callback(gpointer) {
    wpe_android_view_backend_exportable_dispatch_frame_complete(s_viewBackendExportable);
    return G_SOURCE_REMOVE;
}

void wpe_browser_glue_frame_complete() {
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, frame_complete_callback, NULL, NULL);
    g_source_set_priority(source, G_PRIORITY_HIGH + 30);
    g_source_attach(source, uiProcessThreadContext);
    g_source_unref(source);
}

static gboolean touch_event_callback(gpointer data) {
    struct wpe_input_touch_event_raw *touchEventRaw = reinterpret_cast<struct wpe_input_touch_event_raw *>(data);

    struct wpe_input_touch_event touchEvent;
    touchEvent.touchpoints = touchEventRaw;
    touchEvent.touchpoints_length = 1;
    touchEvent.type = touchEventRaw->type;
    touchEvent.id = 0;
    touchEvent.time = touchEventRaw->time;

    struct wpe_view_backend *viewBackend = wpe_android_view_backend_exportable_get_view_backend(
            s_viewBackendExportable);
    wpe_view_backend_dispatch_touch_event(viewBackend, &touchEvent);

    return G_SOURCE_REMOVE;
}

void wpe_browser_glue_touch_event(jlong time, jint type, jfloat x, jfloat y) {
    wpe_input_touch_event_type touchEventType = wpe_input_touch_event_type_null;
    switch (type) {
        case 0:
            touchEventType = wpe_input_touch_event_type_down;
            break;
        case 1:
            touchEventType = wpe_input_touch_event_type_motion;
            break;
        case 2:
            touchEventType = wpe_input_touch_event_type_up;
            break;
    }

    struct wpe_input_touch_event_raw *touchEventRaw = g_new0(struct wpe_input_touch_event_raw, 1);
    touchEventRaw->type = touchEventType;
    touchEventRaw->time = (uint32_t) time;
    touchEventRaw->id = 0;
    touchEventRaw->x = (int32_t) x;
    touchEventRaw->y = (int32_t) y;

    GSource *source = g_idle_source_new();
    g_source_set_callback(source, touch_event_callback, touchEventRaw, g_free);
    g_source_set_priority(source, G_PRIORITY_HIGH + 30);
    g_source_attach(source, uiProcessThreadContext);
    g_source_unref(source);
}