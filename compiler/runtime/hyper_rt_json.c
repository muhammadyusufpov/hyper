#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KIND_I64 = 0,
    KIND_F64 = 1,
    KIND_STR = 2,
    KIND_BOOL = 3,
    KIND_NONE = 4,
    KIND_LIST = 5,
    KIND_DICT = 6
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

typedef struct {
    RtDictEntry *entries;
    size_t len;
    size_t cap;
    /* Hash index lives in hyper_rt.c; dump only walks `entries` in insert order. */
    int32_t *slots;
    size_t nslots;
} RtDict;

extern int64_t hyper_rt_list_new(void);
extern void hyper_rt_list_push(int64_t list, int64_t value, int64_t kind);
extern int64_t hyper_rt_dict_new(void);
extern void hyper_rt_dict_push(int64_t dict, int64_t key, int64_t key_kind, int64_t val, int64_t val_kind);
extern int64_t hyper_rt_file_read_all(
    int64_t handle,
    int64_t handle_kind,
    int64_t line,
    int64_t line_kind
);
extern int64_t hyper_rt_file_write(
    int64_t handle,
    int64_t handle_kind,
    int64_t text,
    int64_t text_kind,
    int64_t line,
    int64_t line_kind
);

static void hyper_rt_runtime_error(int64_t line, const char *msg) {
    fflush(stdout);
    fprintf(stderr, "RuntimeError: line %lld: %s\n", (long long)line, msg);
    exit(70);
}

static char *rt_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (out) {
        memcpy(out, s, n);
    }
    return out;
}

static int64_t cstr_payload(const char *text) {
    return (int64_t)(intptr_t)rt_strdup(text ? text : "");
}

static char *str_arg(int64_t payload, int64_t kind, int64_t line, const char *context) {
    if (kind != KIND_STR) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: expected a string", context);
        hyper_rt_runtime_error(line, buf);
    }
    if (payload == 0) {
        return rt_strdup("");
    }
    return rt_strdup((const char *)(intptr_t)payload);
}

static size_t indent_arg(int64_t payload, int64_t kind) {
    if (kind == KIND_I64 && payload > 0) {
        return (size_t)payload;
    }
    return 0;
}

static double bits_to_double(int64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static int64_t double_to_bits(double d) {
    int64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

typedef struct {
    const unsigned char *bytes;
    size_t len;
    size_t pos;
    char err[256];
    int has_err;
} JsonParser;

static void parser_fail(JsonParser *p, const char *msg) {
    if (!p->has_err) {
        snprintf(p->err, sizeof(p->err), "%s", msg);
        p->has_err = 1;
    }
}

static void parser_failf(JsonParser *p, const char *fmt, ...) {
    if (p->has_err) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
    p->has_err = 1;
}

static int parser_peek(const JsonParser *p) {
    if (p->pos >= p->len) {
        return -1;
    }
    return (int)p->bytes[p->pos];
}

static void parser_skip_ws(JsonParser *p) {
    while (parser_peek(p) >= 0) {
        int c = parser_peek(p);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static int parser_expect(JsonParser *p, unsigned char byte) {
    if (parser_peek(p) == (int)byte) {
        p->pos++;
        return 0;
    }
    parser_failf(p, "expected '%c' at byte %zu", byte, p->pos);
    return -1;
}

static int parser_literal(JsonParser *p, const char *word) {
    size_t n = strlen(word);
    if (p->pos + n <= p->len && memcmp(p->bytes + p->pos, word, n) == 0) {
        p->pos += n;
        return 0;
    }
    parser_failf(p, "invalid literal at byte %zu", p->pos);
    return -1;
}

static int parser_parse_hex4(JsonParser *p, unsigned int *out) {
    if (p->pos + 4 > p->len) {
        parser_fail(p, "truncated unicode escape");
        return -1;
    }
    unsigned int value = 0;
    for (size_t i = 0; i < 4; i++) {
        unsigned char c = p->bytes[p->pos + i];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            value |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            value |= c - 'A' + 10;
        } else {
            parser_fail(p, "invalid unicode escape");
            return -1;
        }
    }
    p->pos += 4;
    *out = value;
    return 0;
}

static int parser_append_utf8(char **out, size_t *len, size_t *cap, unsigned int codepoint) {
    unsigned char buf[4];
    size_t n = 0;
    if (codepoint <= 0x7f) {
        buf[0] = (unsigned char)codepoint;
        n = 1;
    } else if (codepoint <= 0x7ff) {
        buf[0] = (unsigned char)(0xc0 | (codepoint >> 6));
        buf[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
        n = 2;
    } else if (codepoint <= 0xffff) {
        buf[0] = (unsigned char)(0xe0 | (codepoint >> 12));
        buf[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        buf[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
        n = 3;
    } else if (codepoint <= 0x10ffff) {
        buf[0] = (unsigned char)(0xf0 | (codepoint >> 18));
        buf[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
        buf[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        buf[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
        n = 4;
    } else {
        return -1;
    }
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        while (*len + n + 1 > ncap) {
            ncap *= 2;
        }
        char *ni = (char *)realloc(*out, ncap);
        if (!ni) {
            return -1;
        }
        *out = ni;
        *cap = ncap;
    }
    memcpy(*out + *len, buf, n);
    *len += n;
    (*out)[*len] = '\0';
    return 0;
}

static int parser_parse_unicode_escape(JsonParser *p, char **out, size_t *len, size_t *cap) {
    unsigned int high = 0;
    if (parser_parse_hex4(p, &high) != 0) {
        return -1;
    }
    if (high >= 0xd800 && high < 0xdc00) {
        if (parser_peek(p) == '\\' && p->pos + 1 < p->len && p->bytes[p->pos + 1] == 'u') {
            p->pos += 2;
            unsigned int low = 0;
            if (parser_parse_hex4(p, &low) != 0) {
                return -1;
            }
            if (low >= 0xdc00 && low < 0xe000) {
                unsigned int combined =
                    0x10000u + (((high - 0xd800u) << 10) + (low - 0xdc00u));
                return parser_append_utf8(out, len, cap, combined);
            }
            parser_fail(p, "invalid low surrogate");
            return -1;
        }
        parser_fail(p, "lone high surrogate");
        return -1;
    }
    return parser_append_utf8(out, len, cap, high);
}

static int parser_parse_string(JsonParser *p, char **out_text) {
    if (parser_expect(p, '"') != 0) {
        return -1;
    }
    size_t start = p->pos;
    char *out = NULL;
    size_t len = 0;
    size_t cap = 0;

    while (parser_peek(p) >= 0) {
        unsigned char c = (unsigned char)parser_peek(p);
        if (c == '"') {
            size_t raw_len = p->pos - start;
            out = (char *)malloc(raw_len + 1);
            if (!out) {
                parser_fail(p, "out of memory");
                return -1;
            }
            memcpy(out, p->bytes + start, raw_len);
            out[raw_len] = '\0';
            p->pos++;
            *out_text = out;
            return 0;
        }
        if (c == '\\') {
            break;
        }
        p->pos++;
    }

    size_t prefix_len = p->pos - start;
    if (prefix_len > 0) {
        out = (char *)malloc(prefix_len + 1);
        if (!out) {
            parser_fail(p, "out of memory");
            return -1;
        }
        memcpy(out, p->bytes + start, prefix_len);
        len = prefix_len;
        out[len] = '\0';
        cap = prefix_len + 1;
    }

    for (;;) {
        int c = parser_peek(p);
        if (c < 0) {
            free(out);
            parser_fail(p, "unterminated string");
            return -1;
        }
        if (c == '"') {
            p->pos++;
            *out_text = out;
            return 0;
        }
        if (c == '\\') {
            p->pos++;
            int esc = parser_peek(p);
            if (esc < 0) {
                free(out);
                parser_fail(p, "unterminated escape");
                return -1;
            }
            p->pos++;
            switch (esc) {
            case '"':
                if (parser_append_utf8(&out, &len, &cap, '"') != 0) {
                    goto oom;
                }
                break;
            case '\\':
                if (parser_append_utf8(&out, &len, &cap, '\\') != 0) {
                    goto oom;
                }
                break;
            case '/':
                if (parser_append_utf8(&out, &len, &cap, '/') != 0) {
                    goto oom;
                }
                break;
            case 'b':
                if (parser_append_utf8(&out, &len, &cap, 0x08) != 0) {
                    goto oom;
                }
                break;
            case 'f':
                if (parser_append_utf8(&out, &len, &cap, 0x0c) != 0) {
                    goto oom;
                }
                break;
            case 'n':
                if (parser_append_utf8(&out, &len, &cap, '\n') != 0) {
                    goto oom;
                }
                break;
            case 'r':
                if (parser_append_utf8(&out, &len, &cap, '\r') != 0) {
                    goto oom;
                }
                break;
            case 't':
                if (parser_append_utf8(&out, &len, &cap, '\t') != 0) {
                    goto oom;
                }
                break;
            case 'u':
                if (parser_parse_unicode_escape(p, &out, &len, &cap) != 0) {
                    free(out);
                    return -1;
                }
                break;
            default:
                free(out);
                parser_failf(p, "invalid escape '\\%c'", esc);
                return -1;
            }
            continue;
        }
        size_t chunk_start = p->pos;
        while (parser_peek(p) >= 0) {
            int b = parser_peek(p);
            if (b == '"' || b == '\\') {
                break;
            }
            p->pos++;
        }
        size_t chunk_len = p->pos - chunk_start;
        if (len + chunk_len + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 64;
            while (len + chunk_len + 1 > ncap) {
                ncap *= 2;
            }
            char *ni = (char *)realloc(out, ncap);
            if (!ni) {
                goto oom;
            }
            out = ni;
            cap = ncap;
        }
        memcpy(out + len, p->bytes + chunk_start, chunk_len);
        len += chunk_len;
        out[len] = '\0';
    }

oom:
    free(out);
    parser_fail(p, "out of memory");
    return -1;
}

static int parser_parse_number(JsonParser *p, RtValue *out) {
    size_t start = p->pos;
    int c = parser_peek(p);
    if (c == '-' || c == '+') {
        p->pos++;
    }
    int is_float = 0;
    while (parser_peek(p) >= 0) {
        c = parser_peek(p);
        if (c >= '0' && c <= '9') {
            p->pos++;
        } else if (c == '.' || c == 'e' || c == 'E') {
            is_float = 1;
            p->pos++;
        } else if (c == '-' || c == '+') {
            p->pos++;
        } else {
            break;
        }
    }
    if (p->pos == start) {
        parser_failf(p, "expected a value at byte %zu", start);
        return -1;
    }
    char buf[128];
    size_t n = p->pos - start;
    if (n >= sizeof(buf)) {
        parser_fail(p, "invalid number");
        return -1;
    }
    memcpy(buf, p->bytes + start, n);
    buf[n] = '\0';

    if (!is_float) {
        char *end = NULL;
        long long iv = strtoll(buf, &end, 10);
        if (end == buf + (ptrdiff_t)(n)) {
            out->kind = KIND_I64;
            out->payload = (int64_t)iv;
            return 0;
        }
    }
    char *end = NULL;
    double dv = strtod(buf, &end);
    if (end != buf + (ptrdiff_t)(n)) {
        parser_failf(p, "invalid number '%s'", buf);
        return -1;
    }
    out->kind = KIND_F64;
    out->payload = double_to_bits(dv);
    return 0;
}

static int parser_parse_value(JsonParser *p, RtValue *out);

static int parser_parse_array(JsonParser *p, RtValue *out) {
    if (parser_expect(p, '[') != 0) {
        return -1;
    }
    int64_t list = hyper_rt_list_new();
    parser_skip_ws(p);
    if (parser_peek(p) == ']') {
        p->pos++;
        out->kind = KIND_LIST;
        out->payload = list;
        return 0;
    }
    for (;;) {
        parser_skip_ws(p);
        RtValue item;
        if (parser_parse_value(p, &item) != 0) {
            return -1;
        }
        hyper_rt_list_push(list, item.payload, item.kind);
        parser_skip_ws(p);
        int c = parser_peek(p);
        if (c == ',') {
            p->pos++;
        } else if (c == ']') {
            p->pos++;
            out->kind = KIND_LIST;
            out->payload = list;
            return 0;
        } else {
            parser_failf(p, "expected ',' or ']' at byte %zu", p->pos);
            return -1;
        }
    }
}

static int parser_parse_object(JsonParser *p, RtValue *out) {
    if (parser_expect(p, '{') != 0) {
        return -1;
    }
    int64_t dict = hyper_rt_dict_new();
    parser_skip_ws(p);
    if (parser_peek(p) == '}') {
        p->pos++;
        out->kind = KIND_DICT;
        out->payload = dict;
        return 0;
    }
    for (;;) {
        char *key = NULL;
        parser_skip_ws(p);
        if (parser_parse_string(p, &key) != 0) {
            return -1;
        }
        parser_skip_ws(p);
        if (parser_expect(p, ':') != 0) {
            free(key);
            return -1;
        }
        parser_skip_ws(p);
        RtValue val;
        if (parser_parse_value(p, &val) != 0) {
            free(key);
            return -1;
        }
        hyper_rt_dict_push(dict, cstr_payload(key), KIND_STR, val.payload, val.kind);
        free(key);
        parser_skip_ws(p);
        int c = parser_peek(p);
        if (c == ',') {
            p->pos++;
        } else if (c == '}') {
            p->pos++;
            out->kind = KIND_DICT;
            out->payload = dict;
            return 0;
        } else {
            parser_failf(p, "expected ',' or '}' at byte %zu", p->pos);
            return -1;
        }
    }
}

static int parser_parse_value(JsonParser *p, RtValue *out) {
    int c = parser_peek(p);
    if (c < 0) {
        parser_fail(p, "unexpected end of input");
        return -1;
    }
    switch (c) {
    case '{':
        return parser_parse_object(p, out);
    case '[':
        return parser_parse_array(p, out);
    case '"': {
        char *text = NULL;
        if (parser_parse_string(p, &text) != 0) {
            return -1;
        }
        out->kind = KIND_STR;
        out->payload = cstr_payload(text);
        free(text);
        return 0;
    }
    case 't':
        if (parser_literal(p, "true") != 0) {
            return -1;
        }
        out->kind = KIND_BOOL;
        out->payload = 1;
        return 0;
    case 'f':
        if (parser_literal(p, "false") != 0) {
            return -1;
        }
        out->kind = KIND_BOOL;
        out->payload = 0;
        return 0;
    case 'n':
        if (parser_literal(p, "null") != 0) {
            return -1;
        }
        out->kind = KIND_NONE;
        out->payload = 0;
        return 0;
    default:
        return parser_parse_number(p, out);
    }
}

static int json_parse(const char *source, int64_t line, RtValue *out) {
    JsonParser parser;
    parser.bytes = (const unsigned char *)(source ? source : "");
    parser.len = strlen(source ? source : "");
    parser.pos = 0;
    parser.err[0] = '\0';
    parser.has_err = 0;

    parser_skip_ws(&parser);
    if (parser_parse_value(&parser, out) != 0 || parser.has_err) {
        char buf[320];
        snprintf(buf, sizeof(buf), "invalid JSON: %s", parser.err);
        hyper_rt_runtime_error(line, buf);
    }
    parser_skip_ws(&parser);
    if (parser.pos < parser.len) {
        char buf[128];
        snprintf(buf, sizeof(buf), "invalid JSON: unexpected trailing content at byte %zu", parser.pos);
        hyper_rt_runtime_error(line, buf);
    }
    return 0;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    char err[256];
    int has_err;
} JsonBuffer;

static int buf_reserve(JsonBuffer *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) {
        return 0;
    }
    size_t ncap = b->cap ? b->cap * 2 : 64;
    while (b->len + extra + 1 > ncap) {
        ncap *= 2;
    }
    char *ni = (char *)realloc(b->data, ncap);
    if (!ni) {
        snprintf(b->err, sizeof(b->err), "out of memory");
        b->has_err = 1;
        return -1;
    }
    b->data = ni;
    b->cap = ncap;
    return 0;
}

static int buf_append(JsonBuffer *b, const char *text, size_t n) {
    if (buf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, text, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int buf_append_cstr(JsonBuffer *b, const char *text) {
    return buf_append(b, text, strlen(text));
}

static int buf_append_char(JsonBuffer *b, char c) {
    return buf_append(b, &c, 1);
}

static int write_break(JsonBuffer *b, size_t indent, size_t depth) {
    if (indent == 0) {
        return 0;
    }
    if (buf_append_char(b, '\n') != 0) {
        return -1;
    }
    for (size_t i = 0; i < indent * depth; i++) {
        if (buf_append_char(b, ' ') != 0) {
            return -1;
        }
    }
    return 0;
}

static int write_string(JsonBuffer *b, const char *text) {
    if (buf_append_char(b, '"') != 0) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '"':
            if (buf_append_cstr(b, "\\\"") != 0) {
                return -1;
            }
            break;
        case '\\':
            if (buf_append_cstr(b, "\\\\") != 0) {
                return -1;
            }
            break;
        case '\n':
            if (buf_append_cstr(b, "\\n") != 0) {
                return -1;
            }
            break;
        case '\r':
            if (buf_append_cstr(b, "\\r") != 0) {
                return -1;
            }
            break;
        case '\t':
            if (buf_append_cstr(b, "\\t") != 0) {
                return -1;
            }
            break;
        case 0x08:
            if (buf_append_cstr(b, "\\b") != 0) {
                return -1;
            }
            break;
        case 0x0c:
            if (buf_append_cstr(b, "\\f") != 0) {
                return -1;
            }
            break;
        default:
            if (*p < 0x20) {
                char esc[7];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned int)*p);
                if (buf_append_cstr(b, esc) != 0) {
                    return -1;
                }
            } else {
                if (buf_append_char(b, (char)*p) != 0) {
                    return -1;
                }
            }
            break;
        }
    }
    return buf_append_char(b, '"');
}

static int write_float(JsonBuffer *b, double value) {
    if (!isfinite(value)) {
        snprintf(b->err, sizeof(b->err), "cannot serialize NaN or infinity to JSON");
        b->has_err = 1;
        return -1;
    }
    char num[64];
    if (value == trunc(value) && fabs(value) < 1e15) {
        snprintf(num, sizeof(num), "%.1f", value);
    } else {
        snprintf(num, sizeof(num), "%.17g", value);
    }
    return buf_append_cstr(b, num);
}

static int write_value(JsonBuffer *b, int64_t payload, int64_t kind, size_t indent, size_t depth);

static int write_array(JsonBuffer *b, const RtList *list, size_t indent, size_t depth) {
    if (list->len == 0) {
        return buf_append_cstr(b, "[]");
    }
    if (buf_append_char(b, '[') != 0) {
        return -1;
    }
    for (size_t i = 0; i < list->len; i++) {
        if (i > 0 && buf_append_char(b, ',') != 0) {
            return -1;
        }
        if (write_break(b, indent, depth + 1) != 0) {
            return -1;
        }
        if (write_value(b, list->items[i].payload, list->items[i].kind, indent, depth + 1) != 0) {
            return -1;
        }
    }
    if (write_break(b, indent, depth) != 0) {
        return -1;
    }
    return buf_append_char(b, ']');
}

static int write_object(JsonBuffer *b, const RtDict *dict, size_t indent, size_t depth) {
    if (dict->len == 0) {
        return buf_append_cstr(b, "{}");
    }
    if (buf_append_char(b, '{') != 0) {
        return -1;
    }
    for (size_t i = 0; i < dict->len; i++) {
        if (i > 0 && buf_append_char(b, ',') != 0) {
            return -1;
        }
        if (write_break(b, indent, depth + 1) != 0) {
            return -1;
        }
        if (write_string(b, dict->entries[i].key ? dict->entries[i].key : "") != 0) {
            return -1;
        }
        if (buf_append_char(b, ':') != 0) {
            return -1;
        }
        if (indent > 0 && buf_append_char(b, ' ') != 0) {
            return -1;
        }
        if (write_value(
                b,
                dict->entries[i].value.payload,
                dict->entries[i].value.kind,
                indent,
                depth + 1
            ) != 0) {
            return -1;
        }
    }
    if (write_break(b, indent, depth) != 0) {
        return -1;
    }
    return buf_append_char(b, '}');
}

static int write_value(JsonBuffer *b, int64_t payload, int64_t kind, size_t indent, size_t depth) {
    switch (kind) {
    case KIND_NONE:
        return buf_append_cstr(b, "null");
    case KIND_BOOL:
        return buf_append_cstr(b, payload ? "true" : "false");
    case KIND_STR:
        return write_string(b, payload ? (const char *)(intptr_t)payload : "");
    case KIND_I64: {
        char num[32];
        snprintf(num, sizeof(num), "%lld", (long long)payload);
        return buf_append_cstr(b, num);
    }
    case KIND_F64:
        return write_float(b, bits_to_double(payload));
    case KIND_LIST:
        if (payload == 0) {
            return buf_append_cstr(b, "[]");
        }
        return write_array(b, (const RtList *)(intptr_t)payload, indent, depth);
    case KIND_DICT:
        if (payload == 0) {
            return buf_append_cstr(b, "{}");
        }
        return write_object(b, (const RtDict *)(intptr_t)payload, indent, depth);
    default:
        snprintf(b->err, sizeof(b->err), "cannot serialize this value to JSON");
        b->has_err = 1;
        return -1;
    }
}

static char *json_stringify(int64_t payload, int64_t kind, size_t indent, int64_t line) {
    JsonBuffer buf;
    buf.data = NULL;
    buf.len = 0;
    buf.cap = 0;
    buf.err[0] = '\0';
    buf.has_err = 0;

    if (write_value(&buf, payload, kind, indent, 0) != 0 || buf.has_err) {
        free(buf.data);
        hyper_rt_runtime_error(line, buf.err[0] ? buf.err : "cannot serialize this value to JSON");
    }
    return buf.data;
}

int64_t hyper_rt_json_loads(
    int64_t text,
    int64_t text_kind,
    int64_t line,
    int64_t _line_kind,
    int64_t *out_kind
) {
    (void)_line_kind;
    char *source = str_arg(text, text_kind, line, "json.loads");
    RtValue value;
    json_parse(source, line, &value);
    free(source);
    if (out_kind) {
        *out_kind = value.kind;
    }
    return value.payload;
}

int64_t hyper_rt_json_dumps(
    int64_t value,
    int64_t value_kind,
    int64_t indent,
    int64_t indent_kind,
    int64_t line,
    int64_t _line_kind
) {
    (void)_line_kind;
    size_t spaces = indent_arg(indent, indent_kind);
    char *text = json_stringify(value, value_kind, spaces, line);
    int64_t out = cstr_payload(text);
    free(text);
    return out;
}

int64_t hyper_rt_json_load(
    int64_t handle,
    int64_t handle_kind,
    int64_t line,
    int64_t _line_kind,
    int64_t *out_kind
) {
    (void)_line_kind;
    if (handle == 0) {
        hyper_rt_runtime_error(line, "json.load expects an open file");
    }
    int64_t text = hyper_rt_file_read_all(handle, handle_kind, line, _line_kind);
    RtValue value;
    json_parse((const char *)(intptr_t)text, line, &value);
    free((void *)(intptr_t)text);
    if (out_kind) {
        *out_kind = value.kind;
    }
    return value.payload;
}

int64_t hyper_rt_json_dump(
    int64_t value,
    int64_t value_kind,
    int64_t handle,
    int64_t handle_kind,
    int64_t indent,
    int64_t indent_kind,
    int64_t line,
    int64_t _line_kind
) {
    size_t spaces = indent_arg(indent, indent_kind);
    char *text = json_stringify(value, value_kind, spaces, line);
    int64_t n = hyper_rt_file_write(
        handle,
        handle_kind,
        (int64_t)(intptr_t)text,
        KIND_STR,
        line,
        _line_kind
    );
    free(text);
    return n;
}
