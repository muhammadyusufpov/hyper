#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

enum {
    KIND_I64 = 0,
    KIND_F64 = 1,
    KIND_STR = 2,
    KIND_BOOL = 3,
    KIND_NONE = 4,
    KIND_LIST = 5,
    KIND_DICT = 6,
    KIND_STRUCT = 7,
    KIND_FILE = 8,
    KIND_MMAP = 9,
    KIND_U64 = 10
};

typedef struct {
    int64_t kind;
    int64_t payload;
} RtValue;

typedef struct {
    RtValue *items;
    size_t len;
    size_t cap;
} RtList;

typedef struct {
    char *key;
    RtValue value;
} RtDictEntry;

/* Insertion-order entries plus open-addressing index (same ABI as JIT IndexMap). */
#define DICT_EMPTY (-1)

typedef struct {
    RtDictEntry *entries;
    size_t len;
    size_t cap;
    int32_t *slots;
    size_t nslots;
} RtDict;

static uint64_t dict_hash(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        h ^= *p++;
        h *= 1099511628211ULL;
    }
    return h;
}

static const char *dict_key_cstr(const char *key) {
    return key ? key : "";
}

static int dict_rehash(RtDict *dict, size_t nslots) {
    int32_t *slots = (int32_t *)malloc(nslots * sizeof(int32_t));
    if (!slots) {
        return 0;
    }
    for (size_t i = 0; i < nslots; i++) {
        slots[i] = DICT_EMPTY;
    }
    uint64_t mask = (uint64_t)nslots - 1;
    for (size_t i = 0; i < dict->len; i++) {
        uint64_t h = dict_hash(dict_key_cstr(dict->entries[i].key));
        for (;;) {
            size_t slot = (size_t)(h & mask);
            if (slots[slot] == DICT_EMPTY) {
                slots[slot] = (int32_t)i;
                break;
            }
            h++;
        }
    }
    free(dict->slots);
    dict->slots = slots;
    dict->nslots = nslots;
    return 1;
}

static int32_t dict_lookup(const RtDict *dict, const char *k) {
    if (!dict->nslots) {
        return DICT_EMPTY;
    }
    const char *want = dict_key_cstr(k);
    uint64_t mask = (uint64_t)dict->nslots - 1;
    uint64_t h = dict_hash(want);
    for (size_t i = 0; i < dict->nslots; i++) {
        size_t slot = (size_t)(h & mask);
        int32_t idx = dict->slots[slot];
        if (idx == DICT_EMPTY) {
            return DICT_EMPTY;
        }
        if (strcmp(dict_key_cstr(dict->entries[idx].key), want) == 0) {
            return idx;
        }
        h++;
    }
    return DICT_EMPTY;
}

static int dict_index_new_entry(RtDict *dict) {
    if (!dict->nslots || dict->len * 4 > dict->nslots * 3) {
        size_t nslots = dict->nslots ? dict->nslots * 2 : 8;
        while (dict->len * 4 > nslots * 3) {
            nslots *= 2;
        }
        return dict_rehash(dict, nslots);
    }
    int32_t idx = (int32_t)(dict->len - 1);
    uint64_t mask = (uint64_t)dict->nslots - 1;
    uint64_t h = dict_hash(dict_key_cstr(dict->entries[idx].key));
    for (;;) {
        size_t slot = (size_t)(h & mask);
        if (dict->slots[slot] == DICT_EMPTY) {
            dict->slots[slot] = idx;
            return 1;
        }
        h++;
    }
}

static int dict_grow_entries(RtDict *dict) {
    if (dict->len + 1 > dict->cap) {
        size_t ncap = dict->cap ? dict->cap * 2 : 4;
        RtDictEntry *ne = (RtDictEntry *)realloc(dict->entries, ncap * sizeof(RtDictEntry));
        if (!ne) {
            return 0;
        }
        dict->entries = ne;
        dict->cap = ncap;
    }
    return 1;
}

typedef struct {
    RtValue *fields;
    size_t len;
} RtStruct;

static void free_rt_value(RtValue v);

static void free_rt_list(RtList *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->len; i++) {
        free_rt_value(list->items[i]);
    }
    free(list->items);
    free(list);
}

static void free_rt_dict(RtDict *dict) {
    if (!dict) {
        return;
    }
    for (size_t i = 0; i < dict->len; i++) {
        free(dict->entries[i].key);
        free_rt_value(dict->entries[i].value);
    }
    free(dict->entries);
    free(dict->slots);
    free(dict);
}

static void free_rt_struct(RtStruct *st) {
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->len; i++) {
        free_rt_value(st->fields[i]);
    }
    free(st->fields);
    free(st);
}

static void free_rt_value(RtValue v) {
    switch (v.kind) {
    case KIND_STR:
        if (v.payload) {
            free((void *)(intptr_t)v.payload);
        }
        break;
    case KIND_LIST:
        free_rt_list((RtList *)(intptr_t)v.payload);
        break;
    case KIND_DICT:
        free_rt_dict((RtDict *)(intptr_t)v.payload);
        break;
    case KIND_STRUCT:
        free_rt_struct((RtStruct *)(intptr_t)v.payload);
        break;
    default:
        break;
    }
}

extern int64_t __main__(void);
void hyper_rt_div_by_zero(int64_t line);

void hyper_rt_print_i64(int64_t v) {
    printf("%lld", (long long)v);
}

/* Match Rust's Display for f64: the shortest digit string that round-trips,
   always written in positional notation. `%g` would round to six digits. */
static void format_double(double v, char *out, size_t cap) {
    if (v != v) {
        snprintf(out, cap, "NaN");
        return;
    }
    if (v == INFINITY || v == -INFINITY) {
        snprintf(out, cap, v > 0 ? "inf" : "-inf");
        return;
    }

    char sci[64];
    int prec;
    for (prec = 0; prec <= 17; prec++) {
        snprintf(sci, sizeof(sci), "%.*e", prec, v);
        if (strtod(sci, NULL) == v) {
            break;
        }
    }
    if (prec > 17) {
        snprintf(sci, sizeof(sci), "%.17e", v);
    }

    const char *p = sci;
    int negative = 0;
    if (*p == '-') {
        negative = 1;
        p++;
    }
    char digits[32];
    size_t ndigits = 0;
    while (*p && *p != 'e' && *p != 'E') {
        if (*p >= '0' && *p <= '9' && ndigits + 1 < sizeof(digits)) {
            digits[ndigits++] = *p;
        }
        p++;
    }
    digits[ndigits] = '\0';
    int exp10 = (*p == 'e' || *p == 'E') ? atoi(p + 1) : 0;
    while (ndigits > 1 && digits[ndigits - 1] == '0') {
        digits[--ndigits] = '\0';
    }

    size_t pos = 0;
    if (negative && pos + 1 < cap) {
        out[pos++] = '-';
    }
    int point = exp10 + 1;
    if (point <= 0) {
        if (pos + 2 < cap) {
            out[pos++] = '0';
            out[pos++] = '.';
        }
        for (int i = 0; i < -point && pos + 1 < cap; i++) {
            out[pos++] = '0';
        }
        for (size_t i = 0; i < ndigits && pos + 1 < cap; i++) {
            out[pos++] = digits[i];
        }
    } else if ((size_t)point >= ndigits) {
        for (size_t i = 0; i < ndigits && pos + 1 < cap; i++) {
            out[pos++] = digits[i];
        }
        for (size_t i = ndigits; i < (size_t)point && pos + 1 < cap; i++) {
            out[pos++] = '0';
        }
    } else {
        for (size_t i = 0; i < ndigits && pos + 1 < cap; i++) {
            if (i == (size_t)point && pos + 1 < cap) {
                out[pos++] = '.';
            }
            if (pos + 1 < cap) {
                out[pos++] = digits[i];
            }
        }
    }
    out[pos] = '\0';
}

void hyper_rt_print_f64(double v) {
    char buf[512];
    format_double(v, buf, sizeof(buf));
    fputs(buf, stdout);
}

void hyper_rt_print_str(const char *s) {
    if (!s) {
        return;
    }
    printf("%s", s);
}

void hyper_rt_print_newline(void) {
    printf("\n");
}

/* Separator emitted between `print` arguments (interpreter joins with a space). */
void hyper_rt_print_separator(void) {
    putchar(' ');
}

int64_t hyper_rt_pow_i64(int64_t base, int64_t exp) {
    if (exp < 0) {
        return 0;
    }
    int64_t result = 1;
    uint64_t e = (uint64_t)exp;
    int64_t b = base;
    while (e > 0) {
        if (e & 1) {
            result *= b;
        }
        b *= b;
        e >>= 1;
    }
    return result;
}

double hyper_rt_pow_f64(double base, double exp) {
    return pow(base, exp);
}

int64_t hyper_rt_clock(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        return 0;
    }
    double secs = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    union {
        double d;
        uint64_t u;
    } bits;
    bits.d = secs;
    return (int64_t)bits.u;
}

int64_t hyper_rt_floor_div_i64(int64_t a, int64_t b) {
    if (b == 0) {
        hyper_rt_div_by_zero(0);
        return 0;
    }
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((a < 0) ^ (b < 0))) {
        q -= 1;
    }
    return q;
}

double hyper_rt_floor_div_f64(double a, double b) {
    if (b == 0.0) {
        hyper_rt_div_by_zero(0);
        return 0.0;
    }
    return floor(a / b);
}

static void format_value(const RtValue *v);

static void format_list(const RtList *list) {
    putchar('[');
    for (size_t i = 0; i < list->len; i++) {
        if (i > 0) {
            printf(", ");
        }
        format_value(&list->items[i]);
    }
    putchar(']');
}

static void format_dict(const RtDict *dict) {
    putchar('{');
    for (size_t i = 0; i < dict->len; i++) {
        if (i > 0) {
            printf(", ");
        }
        printf("%s: ", dict->entries[i].key ? dict->entries[i].key : "");
        format_value(&dict->entries[i].value);
    }
    putchar('}');
}

static void format_struct(const RtStruct *st) {
    putchar('{');
    for (size_t i = 0; i < st->len; i++) {
        if (i > 0) {
            printf(", ");
        }
        format_value(&st->fields[i]);
    }
    putchar('}');
}

static void format_value(const RtValue *v) {
    switch (v->kind) {
    case KIND_I64:
        printf("%lld", (long long)v->payload);
        break;
    case KIND_U64:
        printf("%llu", (unsigned long long)(uint64_t)v->payload);
        break;
    case KIND_F64: {
        double d;
        char buf[512];
        memcpy(&d, &v->payload, sizeof(d));
        format_double(d, buf, sizeof(buf));
        fputs(buf, stdout);
        break;
    }
    case KIND_STR:
        if (v->payload) {
            fputs((const char *)(intptr_t)v->payload, stdout);
        }
        break;
    case KIND_BOOL:
        fputs(v->payload ? "true" : "false", stdout);
        break;
    case KIND_NONE:
        fputs("None", stdout);
        break;
    case KIND_LIST:
        if (v->payload) {
            format_list((const RtList *)(intptr_t)v->payload);
        } else {
            fputs("[]", stdout);
        }
        break;
    case KIND_DICT:
        if (v->payload) {
            format_dict((const RtDict *)(intptr_t)v->payload);
        } else {
            fputs("{}", stdout);
        }
        break;
    case KIND_STRUCT:
        if (v->payload) {
            format_struct((const RtStruct *)(intptr_t)v->payload);
        } else {
            fputs("{}", stdout);
        }
        break;
    case KIND_FILE:
        fputs("<file>", stdout);
        break;
    case KIND_MMAP:
        fputs("<mmap file>", stdout);
        break;
    default:
        fputs("<?>", stdout);
        break;
    }
}

int64_t hyper_rt_list_new(void) {
    RtList *list = (RtList *)calloc(1, sizeof(RtList));
    return (int64_t)(intptr_t)list;
}

void hyper_rt_list_push(int64_t list_h, int64_t value, int64_t kind) {
    if (!list_h) {
        return;
    }
    RtList *list = (RtList *)(intptr_t)list_h;
    if (list->len + 1 > list->cap) {
        size_t ncap = list->cap ? list->cap * 2 : 4;
        RtValue *ni = (RtValue *)realloc(list->items, ncap * sizeof(RtValue));
        if (!ni) {
            return;
        }
        list->items = ni;
        list->cap = ncap;
    }
    list->items[list->len].kind = kind;
    list->items[list->len].payload = value;
    list->len++;
}

void hyper_rt_print_list(int64_t list_h) {
    if (!list_h) {
        printf("[]");
        return;
    }
    format_list((const RtList *)(intptr_t)list_h);
}

int64_t hyper_rt_dict_new(void) {
    RtDict *dict = (RtDict *)calloc(1, sizeof(RtDict));
    return (int64_t)(intptr_t)dict;
}

static char *key_to_string(int64_t key, int64_t key_kind) {
    if (key_kind == KIND_STR) {
        const char *s = key ? (const char *)(intptr_t)key : "";
        size_t n = strlen(s);
        char *out = (char *)malloc(n + 1);
        if (!out) {
            return NULL;
        }
        memcpy(out, s, n + 1);
        return out;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)key);
    size_t n = strlen(buf);
    char *out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, buf, n + 1);
    return out;
}

void hyper_rt_dict_set(int64_t dict_h, int64_t key, int64_t key_kind, int64_t value, int64_t val_kind);

void hyper_rt_dict_push(int64_t dict_h, int64_t key, int64_t key_kind, int64_t val, int64_t val_kind) {
    hyper_rt_dict_set(dict_h, key, key_kind, val, val_kind);
}

void hyper_rt_print_dict(int64_t dict_h) {
    if (!dict_h) {
        printf("{}");
        return;
    }
    format_dict((const RtDict *)(intptr_t)dict_h);
}

void hyper_rt_print_value(int64_t payload, int64_t kind) {
    RtValue v;
    v.kind = kind;
    v.payload = payload;
    format_value(&v);
}

int64_t hyper_rt_list_get(int64_t list_h, int64_t index, int64_t *out_kind) {
    if (!list_h || !out_kind) {
        return 0;
    }
    const RtList *list = (const RtList *)(intptr_t)list_h;
    if (index < 0 || (size_t)index >= list->len) {
        *out_kind = KIND_NONE;
        return 0;
    }
    *out_kind = list->items[index].kind;
    return list->items[index].payload;
}

void hyper_rt_list_set(int64_t list_h, int64_t index, int64_t value, int64_t kind) {
    if (!list_h) {
        return;
    }
    RtList *list = (RtList *)(intptr_t)list_h;
    if (index < 0 || (size_t)index >= list->len) {
        return;
    }
    free_rt_value(list->items[index]);
    list->items[index].kind = kind;
    list->items[index].payload = value;
}

int64_t hyper_rt_dict_get(int64_t dict_h, int64_t key, int64_t key_kind, int64_t *out_kind) {
    if (!dict_h || !out_kind) {
        return 0;
    }
    const RtDict *dict = (const RtDict *)(intptr_t)dict_h;
    /* String keys are already C strings — compare without malloc. */
    char *owned = NULL;
    const char *k;
    if (key_kind == KIND_STR) {
        k = key ? (const char *)(intptr_t)key : "";
    } else {
        owned = key_to_string(key, key_kind);
        if (!owned) {
            *out_kind = KIND_NONE;
            return 0;
        }
        k = owned;
    }
    int32_t idx = dict_lookup(dict, k);
    free(owned);
    if (idx >= 0) {
        *out_kind = dict->entries[idx].value.kind;
        return dict->entries[idx].value.payload;
    }
    *out_kind = KIND_NONE;
    return 0;
}

void hyper_rt_dict_set(int64_t dict_h, int64_t key, int64_t key_kind, int64_t value, int64_t val_kind) {
    if (!dict_h) {
        return;
    }
    RtDict *dict = (RtDict *)(intptr_t)dict_h;
    char *owned = NULL;
    const char *k;
    if (key_kind == KIND_STR) {
        k = key ? (const char *)(intptr_t)key : "";
    } else {
        owned = key_to_string(key, key_kind);
        if (!owned) {
            return;
        }
        k = owned;
    }
    int32_t idx = dict_lookup(dict, k);
    if (idx >= 0) {
        free(owned);
        free_rt_value(dict->entries[idx].value);
        dict->entries[idx].value.kind = val_kind;
        dict->entries[idx].value.payload = value;
        return;
    }
    /* Insert path still needs an owned key copy. */
    if (key_kind == KIND_STR) {
        owned = key_to_string(key, key_kind);
        if (!owned) {
            return;
        }
    }
    if (!dict_grow_entries(dict)) {
        free(owned);
        return;
    }
    dict->entries[dict->len].key = owned;
    dict->entries[dict->len].value.kind = val_kind;
    dict->entries[dict->len].value.payload = value;
    dict->len += 1;
    if (!dict_index_new_entry(dict)) {
        dict->len -= 1;
        free(owned);
        dict->entries[dict->len].key = NULL;
    }
}

int64_t hyper_rt_index_get(int64_t obj, int64_t obj_kind, int64_t idx, int64_t idx_kind, int64_t *out_kind) {
    if (obj_kind == KIND_DICT) {
        return hyper_rt_dict_get(obj, idx, idx_kind, out_kind);
    }
    if (obj_kind == KIND_LIST) {
        return hyper_rt_list_get(obj, idx, out_kind);
    }
    if (out_kind) {
        *out_kind = KIND_NONE;
    }
    return 0;
}

void hyper_rt_index_set(int64_t obj, int64_t obj_kind, int64_t idx, int64_t idx_kind, int64_t value, int64_t val_kind) {
    if (obj_kind == KIND_DICT) {
        hyper_rt_dict_set(obj, idx, idx_kind, value, val_kind);
    } else if (obj_kind == KIND_LIST) {
        hyper_rt_list_set(obj, idx, value, val_kind);
    }
}

int64_t hyper_rt_list_len(int64_t list_h) {
    if (!list_h) {
        return 0;
    }
    const RtList *list = (const RtList *)(intptr_t)list_h;
    return (int64_t)list->len;
}

static void rt_fatal(int64_t line, const char *msg) {
    fflush(stdout);
    fprintf(stderr, "RuntimeError: line %lld: %s\n", (long long)line, msg);
    exit(70);
}

static int64_t utf8_char_len(int64_t payload) {
    const char *s = payload ? (const char *)(intptr_t)payload : "";
    int64_t n = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0) != 0x80) {
            n++;
        }
        s++;
    }
    return n;
}

int64_t hyper_rt_coll_len(int64_t payload, int64_t kind, int64_t line, int64_t line_kind) {
    (void)line_kind;
    if (kind == KIND_LIST) {
        return hyper_rt_list_len(payload);
    }
    if (kind == KIND_DICT) {
        if (!payload) {
            return 0;
        }
        return (int64_t)((const RtDict *)(intptr_t)payload)->len;
    }
    if (kind == KIND_STR) {
        return utf8_char_len(payload);
    }
    rt_fatal(line, "this type has no method 'len'");
    return 0;
}

void hyper_rt_coll_append(
    int64_t payload,
    int64_t kind,
    int64_t value,
    int64_t val_kind,
    int64_t line,
    int64_t line_kind
) {
    (void)line_kind;
    if (kind == KIND_LIST) {
        hyper_rt_list_push(payload, value, val_kind);
        return;
    }
    if (kind == KIND_DICT) {
        rt_fatal(line, "dict has no method 'append'");
    }
    rt_fatal(line, "this type has no method 'append'");
}

int64_t hyper_rt_coll_keys(int64_t payload, int64_t kind, int64_t line, int64_t line_kind) {
    (void)line_kind;
    if (kind != KIND_DICT) {
        if (kind == KIND_LIST) {
            rt_fatal(line, "list has no method 'keys'");
        }
        rt_fatal(line, "this type has no method 'keys'");
    }
    int64_t list = hyper_rt_list_new();
    if (!payload) {
        return list;
    }
    const RtDict *dict = (const RtDict *)(intptr_t)payload;
    for (size_t i = 0; i < dict->len; i++) {
        const char *key = dict->entries[i].key ? dict->entries[i].key : "";
        size_t n = strlen(key);
        char *copy = (char *)malloc(n + 1);
        if (copy) {
            memcpy(copy, key, n + 1);
        }
        hyper_rt_list_push(list, (int64_t)(intptr_t)copy, KIND_STR);
    }
    return list;
}

int64_t hyper_rt_value_to_str(int64_t payload, int64_t kind) {
    RtValue v;
    v.kind = kind;
    v.payload = payload;

    char buf[512];
    switch (kind) {
    case KIND_I64:
        snprintf(buf, sizeof(buf), "%lld", (long long)payload);
        break;
    case KIND_U64:
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(uint64_t)payload);
        break;
    case KIND_F64: {
        double d;
        memcpy(&d, &payload, sizeof(d));
        format_double(d, buf, sizeof(buf));
        break;
    }
    case KIND_STR: {
        const char *s = payload ? (const char *)(intptr_t)payload : "";
        size_t n = strlen(s);
        char *out = (char *)malloc(n + 1);
        if (!out) {
            return 0;
        }
        memcpy(out, s, n + 1);
        return (int64_t)(intptr_t)out;
    }
    case KIND_BOOL:
        snprintf(buf, sizeof(buf), "%s", payload ? "true" : "false");
        break;
    case KIND_NONE:
        snprintf(buf, sizeof(buf), "None");
        break;
    case KIND_LIST:
    case KIND_DICT: {
        /* Fall back to a small fixed buffer via format helpers into temp FILE-less path. */
        snprintf(buf, sizeof(buf), "<?>");
        break;
    }
    default:
        snprintf(buf, sizeof(buf), "<?>");
        break;
    }
    size_t n = strlen(buf);
    char *out = (char *)malloc(n + 1);
    if (!out) {
        return 0;
    }
    memcpy(out, buf, n + 1);
    return (int64_t)(intptr_t)out;
}

int64_t hyper_rt_str_concat(int64_t left, int64_t right) {
    const char *a = left ? (const char *)(intptr_t)left : "";
    const char *b = right ? (const char *)(intptr_t)right : "";
    size_t na = strlen(a);
    size_t nb = strlen(b);
    char *out = (char *)malloc(na + nb + 1);
    if (!out) {
        return 0;
    }
    memcpy(out, a, na);
    memcpy(out + na, b, nb + 1);
    return (int64_t)(intptr_t)out;
}

static double bits_to_double(int64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

/* Mirrors the interpreter's ==: strings, lists and dicts compare by content,
   numbers promote to double when either side is a float, and struct instances
   are never equal to anything. */
static int values_equal(RtValue a, RtValue b) {
    if (a.kind == KIND_STR && b.kind == KIND_STR) {
        const char *x = a.payload ? (const char *)(intptr_t)a.payload : "";
        const char *y = b.payload ? (const char *)(intptr_t)b.payload : "";
        return strcmp(x, y) == 0;
    }
    if (a.kind == KIND_NONE && b.kind == KIND_NONE) {
        return 1;
    }
    if (a.kind == KIND_BOOL && b.kind == KIND_BOOL) {
        return a.payload == b.payload;
    }
    if (a.kind == KIND_F64 || b.kind == KIND_F64) {
        if (a.kind != KIND_F64 && a.kind != KIND_I64) {
            return 0;
        }
        if (b.kind != KIND_F64 && b.kind != KIND_I64) {
            return 0;
        }
        double x = a.kind == KIND_F64 ? bits_to_double(a.payload) : (double)a.payload;
        double y = b.kind == KIND_F64 ? bits_to_double(b.payload) : (double)b.payload;
        return x == y;
    }
    if (a.kind == KIND_I64 && b.kind == KIND_I64) {
        return a.payload == b.payload;
    }
    if (a.kind == KIND_LIST && b.kind == KIND_LIST) {
        if (!a.payload || !b.payload) {
            return a.payload == b.payload;
        }
        const RtList *left = (const RtList *)(intptr_t)a.payload;
        const RtList *right = (const RtList *)(intptr_t)b.payload;
        if (left->len != right->len) {
            return 0;
        }
        for (size_t i = 0; i < left->len; i++) {
            if (!values_equal(left->items[i], right->items[i])) {
                return 0;
            }
        }
        return 1;
    }
    if (a.kind == KIND_DICT && b.kind == KIND_DICT) {
        if (!a.payload || !b.payload) {
            return a.payload == b.payload;
        }
        const RtDict *left = (const RtDict *)(intptr_t)a.payload;
        const RtDict *right = (const RtDict *)(intptr_t)b.payload;
        if (left->len != right->len) {
            return 0;
        }
        for (size_t i = 0; i < left->len; i++) {
            int32_t j = dict_lookup(right, dict_key_cstr(left->entries[i].key));
            if (j < 0 || !values_equal(left->entries[i].value, right->entries[j].value)) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

void hyper_rt_div_by_zero(int64_t line) {
    fflush(stdout);
    fprintf(stderr, "RuntimeError: line %lld: division by zero\n", (long long)line);
    exit(70);
}

static __thread int hyper_rt_handle_depth = 0;
static __thread int hyper_rt_pending_raise = 0;
static __thread char hyper_rt_pending_msg[512];

int64_t hyper_rt_handle_enter(void) {
    hyper_rt_handle_depth++;
    hyper_rt_pending_raise = 0;
    hyper_rt_pending_msg[0] = '\0';
    return 0;
}

int64_t hyper_rt_handle_leave(void) {
    if (hyper_rt_handle_depth > 0) {
        hyper_rt_handle_depth--;
    }
    int pending = hyper_rt_pending_raise;
    hyper_rt_pending_raise = 0;
    return pending ? 1 : 0;
}

static void hyper_rt_format_raise(int64_t payload, int64_t kind, char *buf, size_t buflen) {
    switch (kind) {
        case KIND_I64:
            snprintf(buf, buflen, "%lld", (long long)payload);
            break;
        case KIND_F64: {
            double d;
            memcpy(&d, &payload, sizeof(d));
            snprintf(buf, buflen, "%g", d);
            break;
        }
        case KIND_STR:
            if (payload == 0) {
                buf[0] = '\0';
            } else {
                snprintf(buf, buflen, "%s", (const char *)(intptr_t)payload);
            }
            break;
        case KIND_BOOL:
            snprintf(buf, buflen, "%s", payload ? "true" : "false");
            break;
        case KIND_NONE:
            snprintf(buf, buflen, "None");
            break;
        default:
            snprintf(buf, buflen, "<value>");
            break;
    }
}

int64_t hyper_rt_raise(int64_t payload, int64_t kind, int64_t line, int64_t line_kind) {
    (void)line_kind;
    char msg[512];
    hyper_rt_format_raise(payload, kind, msg, sizeof(msg));
    if (hyper_rt_handle_depth > 0) {
        snprintf(hyper_rt_pending_msg, sizeof(hyper_rt_pending_msg), "%s", msg);
        hyper_rt_pending_raise = 1;
        return 0;
    }
    fflush(stdout);
    fprintf(stderr, "RuntimeError: line %lld: %s\n", (long long)line, msg);
    exit(70);
}

int64_t hyper_rt_value_eq(int64_t a, int64_t a_kind, int64_t b, int64_t b_kind) {
    RtValue left;
    RtValue right;
    left.kind = a_kind;
    left.payload = a;
    right.kind = b_kind;
    right.payload = b;
    return values_equal(left, right) ? 1 : 0;
}

int64_t hyper_rt_struct_new(int64_t nfields) {
    size_t n = nfields < 0 ? 0 : (size_t)nfields;
    RtStruct *st = (RtStruct *)calloc(1, sizeof(RtStruct));
    if (!st) {
        return 0;
    }
    if (n > 0) {
        st->fields = (RtValue *)calloc(n, sizeof(RtValue));
        if (!st->fields) {
            free(st);
            return 0;
        }
        for (size_t i = 0; i < n; i++) {
            st->fields[i].kind = KIND_NONE;
            st->fields[i].payload = 0;
        }
    }
    st->len = n;
    return (int64_t)(intptr_t)st;
}

int64_t hyper_rt_struct_get(int64_t obj, int64_t field, int64_t *out_kind) {
    if (!obj || !out_kind) {
        return 0;
    }
    const RtStruct *st = (const RtStruct *)(intptr_t)obj;
    if (field < 0 || (size_t)field >= st->len) {
        *out_kind = KIND_NONE;
        return 0;
    }
    *out_kind = st->fields[field].kind;
    return st->fields[field].payload;
}

void hyper_rt_struct_set(int64_t obj, int64_t field, int64_t value, int64_t kind) {
    if (!obj) {
        return;
    }
    RtStruct *st = (RtStruct *)(intptr_t)obj;
    if (field < 0 || (size_t)field >= st->len) {
        return;
    }
    free_rt_value(st->fields[field]);
    st->fields[field].kind = kind;
    st->fields[field].payload = value;
}

void hyper_rt_print_struct(int64_t obj) {
    if (!obj) {
        printf("{}");
        return;
    }
    format_struct((const RtStruct *)(intptr_t)obj);
}

int main(void) {
#ifdef _WIN32
    /* Match JIT (Rust) and Unix: keep '\n' as LF, not CRLF, on redirected stdout. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    __main__();
    return 0;
}
