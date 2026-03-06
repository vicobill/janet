#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "janet.h"

typedef enum {
    UJ_NIL = 0,
    UJ_NUMBER = 1,
    UJ_BOOLEAN = 2,
    UJ_STRING = 3
} UJType;

typedef struct {
    int type;
    double number;
    int boolean;
    const char *str;
} UJArg;

typedef UJArg (*UnityCallback)(int32_t argc, const UJArg *args);

typedef struct {
    char *name;
    UnityCallback cb;
} UJEntry;

static JanetTable *s_env = NULL;
static UJEntry *s_entries = NULL;
static int32_t s_entry_count = 0;
static int32_t s_entry_capacity = 0;
static UnityCallback s_log_cb = NULL;

static void registry_add(const char *name, UnityCallback cb) {
    if (s_entry_count + 1 > s_entry_capacity) {
        int32_t newcap = s_entry_capacity == 0 ? 8 : s_entry_capacity * 2;
        UJEntry *newarr = (UJEntry *) realloc(s_entries, sizeof(UJEntry) * newcap);
        if (!newarr) return;
        s_entries = newarr;
        s_entry_capacity = newcap;
    }
    size_t nlen = strlen(name);
    char *dup = (char *) malloc(nlen + 1);
    memcpy(dup, name, nlen + 1);
    s_entries[s_entry_count].name = dup;
    s_entries[s_entry_count].cb = cb;
    s_entry_count++;
}

static UnityCallback registry_find(const char *name) {
    for (int32_t i = 0; i < s_entry_count; i++) {
        if (0 == strcmp(s_entries[i].name, name)) return s_entries[i].cb;
    }
    return NULL;
}

static void registry_clear() {
    for (int32_t i = 0; i < s_entry_count; i++) {
        free(s_entries[i].name);
    }
    free(s_entries);
    s_entries = NULL;
    s_entry_count = 0;
    s_entry_capacity = 0;
}

static UJArg janet_to_ujarg(Janet v) {
    UJArg out;
    int t = janet_type(v);
    switch (t) {
        case JANET_NIL:
            out.type = UJ_NIL;
            out.number = 0.0;
            out.boolean = 0;
            out.str = NULL;
            break;
        case JANET_BOOLEAN:
            out.type = UJ_BOOLEAN;
            out.boolean = janet_unwrap_boolean(v);
            out.number = 0.0;
            out.str = NULL;
            break;
        case JANET_NUMBER:
            out.type = UJ_NUMBER;
            out.number = janet_unwrap_number(v);
            out.boolean = 0;
            out.str = NULL;
            break;
        case JANET_STRING: {
            out.type = UJ_STRING;
            const uint8_t *js = janet_unwrap_string(v);
            out.str = (const char *) js;
            out.number = 0.0;
            out.boolean = (int) janet_string_length(js);;
            break;
        }
        default:
            out.type = UJ_NIL;
            out.number = 0.0;
            out.boolean = 0;
            out.str = NULL;
            break;
    }
    return out;
}

static Janet ujarg_to_janet(const UJArg *a) {
    switch (a->type) {
        case UJ_NIL:
            return janet_wrap_nil();
        case UJ_BOOLEAN:
            return janet_wrap_boolean(a->boolean);
        case UJ_NUMBER:
            return janet_wrap_number(a->number);
        case UJ_STRING:
            return janet_cstringv(a->str ? a->str : "");
        default:
            return janet_wrap_nil();
    }
}

static Janet cfun_uj_invoke(int32_t argc, Janet *argv) {
    janet_fixarity(argc, -1);
    const char *fname = (const char *) janet_getstring(argv, 0);
    UnityCallback cb = registry_find(fname);
    if (!cb) {
        janet_panicf("no managed callback registered for %s", fname);
    }
    int32_t cargc = argc; /* include the fullname as first arg for dispatch */
    UJArg *cargs = (UJArg *) janet_smalloc(sizeof(UJArg) * cargc);
    /* first arg: function fullname string */
    cargs[0].type = UJ_STRING;
    cargs[0].str = fname;
    cargs[0].number = 0.0;
    cargs[0].boolean = 0;
    for (int32_t i = 1; i < cargc; i++) {
        cargs[i] = janet_to_ujarg(argv[i]);
    }
    UJArg ret = cb(cargc, cargs);
    janet_sfree(cargs);
    return ujarg_to_janet(&ret);
}

static Janet cfun_unity_print(int32_t argc, Janet *argv) {
    Janet sym_fmt = janet_csymbolv("string/format");
    Janet fun_fmt = janet_table_get(s_env, sym_fmt);
    if (janet_type(fun_fmt) != JANET_FUNCTION && janet_type(fun_fmt) != JANET_CFUNCTION) {
        return janet_wrap_nil();
    }
    int32_t n = argc + 1;
    Janet *fargv = (Janet *) janet_smalloc(sizeof(Janet) * n);
    int32_t flen = argc * 3;
    uint8_t *fmt = (uint8_t *) janet_smalloc(flen + 1);
    int32_t pos = 0;
    for (int32_t i = 0; i < argc; i++) {
        if (i > 0) fmt[pos++] = ' ';
        fmt[pos++] = '%';
        fmt[pos++] = 'V';
    }
    if (argc == 0) {
        fmt[pos++] = '\0';
        fargv[0] = janet_cstringv((const char *) fmt);
    } else {
        fmt[pos] = '\0';
        fargv[0] = janet_cstringv((const char *) fmt);
    }
    for (int32_t i = 0; i < argc; i++) {
        fargv[i + 1] = argv[i];
    }
    Janet out;
    JanetFunction *fun = janet_unwrap_function(fun_fmt);
    int status = janet_pcall(fun, n, fargv, &out, NULL);
    janet_sfree(fargv);
    janet_sfree(fmt);
    if (status != 0) {
        return janet_wrap_nil();
    }
    if (s_log_cb) {
        UJArg msg;
        msg.type = UJ_STRING;
        const uint8_t *js = janet_unwrap_string(out);
        msg.str = (const char *) js;
        msg.boolean = (int) janet_string_length(js);
        msg.number = 0.0;
        s_log_cb(1, &msg);
    }
    return janet_wrap_nil();
}

EXPORT void JanetUnity_Init() {
    janet_init();
    s_env = janet_core_env(NULL);
    // Register built-in functions
    static const JanetReg bridge_cfuns[] = {
        {"uj/invoke", cfun_uj_invoke, "(uj/invoke name & args)\n\nInvoke a registered managed callback."},
        {"print", cfun_unity_print, "(print & xs)\n\nPrint via managed logger."},
        {NULL, NULL, NULL}
    };
    janet_cfuns(s_env, "unity", bridge_cfuns);
    janet_dostring(s_env, "(def print unity/print)", "unity-print", NULL);
}

EXPORT void JanetUnity_Deinit() {
    registry_clear();
    janet_deinit();
    s_env = NULL;
}

EXPORT void JanetUnity_SetLogCallback(UnityCallback cb) {
    s_log_cb = cb;
}

EXPORT int JanetUnity_DoString(const char *code, const char *source) {
    return janet_dostring(s_env, code, source ? source : "janet_unity", NULL);
}

EXPORT int JanetUnity_DefManaged(const char *ns, const char *name, UnityCallback cb, const char *doc) {
    char fullname[512];
    fullname[0] = '\0';
    if (ns && ns[0]) {
        strncat(fullname, ns, sizeof(fullname) - 1);
        strncat(fullname, "/", sizeof(fullname) - 1);
    }
    strncat(fullname, name, sizeof(fullname) - 1);
    registry_add(fullname, cb);
    char defbuf[1024];
    snprintf(defbuf, sizeof(defbuf),
             "(defn %s [& args] (apply unity/uj/invoke \"%s\" args))",
             fullname, fullname);
    int rc = janet_dostring(s_env, defbuf, "unity-def", NULL);
    (void) doc;
    return rc;
}


EXPORT int JanetUnity_CallByName(const char *fullname, const UJArg *args, int32_t argc, UJArg *out) {
    Janet sym = janet_csymbolv(fullname);
    Janet fun = janet_table_get(s_env, sym);
    if (janet_type(fun) != JANET_FUNCTION && janet_type(fun) != JANET_CFUNCTION) {
        Janet looked = janet_wrap_nil();
        int lr = janet_dostring(s_env, fullname, "lookup", &looked);
        if (lr != 0) return 1;
        fun = looked;
        if (janet_type(fun) != JANET_FUNCTION && janet_type(fun) != JANET_CFUNCTION) return 1;
    }
    Janet *jargv = (Janet *) janet_smalloc(sizeof(Janet) * argc);
    for (int32_t i = 0; i < argc; i++) {
        jargv[i] = ujarg_to_janet(&args[i]);
    }
    Janet ret;
    int status = 1;
    if (janet_type(fun) == JANET_FUNCTION) {
        JanetFunction *fn = janet_unwrap_function(fun);
        status = janet_pcall(fn, argc, jargv, &ret, NULL);
    } else if (janet_type(fun) == JANET_CFUNCTION) {
        JanetFunction *fn = NULL;
        status = 1;
    }
    janet_sfree(jargv);
    if (status == 0 && out) {
        *out = janet_to_ujarg(ret);
    }
    return status;
}
