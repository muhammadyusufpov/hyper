use crate::error;
use indexmap::IndexMap;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;

pub const KIND_I64: i64 = 0;
pub const KIND_F64: i64 = 1;
pub const KIND_STR: i64 = 2;
pub const KIND_BOOL: i64 = 3;
pub const KIND_NONE: i64 = 4;
pub const KIND_LIST: i64 = 5;
pub const KIND_DICT: i64 = 6;
pub const KIND_STRUCT: i64 = 7;
pub const KIND_FILE: i64 = 8;
pub const KIND_MMAP: i64 = 9;
pub const KIND_U64: i64 = 10;

mod file;
mod io;
mod json;
mod mmap;
mod str;
pub use file::{
    hyper_rt_file_close, hyper_rt_file_flush, hyper_rt_file_is_closed, hyper_rt_file_mode,
    hyper_rt_file_open, hyper_rt_file_path, hyper_rt_file_read_all, hyper_rt_file_read_n,
    hyper_rt_file_readline, hyper_rt_file_readlines, hyper_rt_file_seek, hyper_rt_file_size,
    hyper_rt_file_tell, hyper_rt_file_write, hyper_rt_file_writelines,
};
pub use io::hyper_rt_input;
pub use json::{hyper_rt_json_dump, hyper_rt_json_dumps, hyper_rt_json_load, hyper_rt_json_loads};
pub use mmap::{hyper_rt_mmap_close, hyper_rt_mmap_open, hyper_rt_mmap_read_chunk};
pub use str::{
    hyper_rt_str_capitalize, hyper_rt_str_center, hyper_rt_str_count, hyper_rt_str_endswith,
    hyper_rt_str_find, hyper_rt_str_index, hyper_rt_str_isalnum, hyper_rt_str_isalpha,
    hyper_rt_str_isascii, hyper_rt_str_isdigit, hyper_rt_str_islower, hyper_rt_str_isspace,
    hyper_rt_str_istitle, hyper_rt_str_isupper, hyper_rt_str_join, hyper_rt_str_ljust,
    hyper_rt_str_lower, hyper_rt_str_lstrip, hyper_rt_str_partition, hyper_rt_str_removeprefix,
    hyper_rt_str_removesuffix, hyper_rt_str_replace, hyper_rt_str_rfind, hyper_rt_str_rindex,
    hyper_rt_str_rjust, hyper_rt_str_rpartition, hyper_rt_str_rsplit, hyper_rt_str_rstrip,
    hyper_rt_str_split, hyper_rt_str_startswith, hyper_rt_str_strip, hyper_rt_str_swapcase,
    hyper_rt_str_title, hyper_rt_str_upper, hyper_rt_str_zfill,
};

#[derive(Clone)]
pub(crate) struct RtValue {
    kind: i64,
    payload: i64,
}

pub(crate) struct RtList {
    items: Vec<RtValue>,
}

/// Insertion-order map: `IndexMap` so print / `keys()` / JSON stay stable, with
/// amortized O(1) get/set. The AOT runtime (`hyper_rt.c`) uses the same order
/// plus an open-addressing index; ABI (`hyper_rt_dict_*`) is unchanged.
pub(crate) struct RtDict {
    entries: IndexMap<String, RtValue>,
}

struct RtStruct {
    fields: Vec<RtValue>,
}

pub(crate) fn format_value(v: &RtValue) -> String {
    match v.kind {
        KIND_I64 => format!("{}", v.payload),
        KIND_U64 => format!("{}", v.payload as u64),
        KIND_F64 => format!("{}", f64::from_bits(v.payload as u64)),
        KIND_STR => {
            if v.payload == 0 {
                String::new()
            } else {
                let cstr = unsafe { CStr::from_ptr(v.payload as *const c_char) };
                cstr.to_str().unwrap_or("<?>").to_string()
            }
        }
        KIND_BOOL => {
            if v.payload != 0 {
                "true".to_string()
            } else {
                "false".to_string()
            }
        }
        KIND_NONE => "None".to_string(),
        KIND_LIST => {
            let list = unsafe { &*(v.payload as *const RtList) };
            format_list(list)
        }
        KIND_DICT => {
            let dict = unsafe { &*(v.payload as *const RtDict) };
            format_dict(dict)
        }
        KIND_STRUCT => {
            let st = unsafe { &*(v.payload as *const RtStruct) };
            format_struct(st)
        }
        KIND_FILE => "<file>".to_string(),
        KIND_MMAP => "<mmap file>".to_string(),
        _ => format!("<?>"),
    }
}

fn format_list(list: &RtList) -> String {
    use std::fmt::Write;
    let mut out = String::from("[");
    for (i, item) in list.items.iter().enumerate() {
        if i > 0 {
            out.push_str(", ");
        }
        let _ = write!(out, "{}", format_value(item));
    }
    out.push(']');
    out
}

fn format_dict(dict: &RtDict) -> String {
    use std::fmt::Write;
    let mut out = String::from("{");
    for (i, (k, v)) in dict.entries.iter().enumerate() {
        if i > 0 {
            out.push_str(", ");
        }
        let _ = write!(out, "{}: {}", k, format_value(v));
    }
    out.push('}');
    out
}

fn dict_own_key(key: i64, key_kind: i64, owned: String) -> String {
    if key_kind == KIND_STR {
        key_to_string(key, key_kind)
    } else {
        owned
    }
}

fn format_struct(st: &RtStruct) -> String {
    use std::fmt::Write;
    let mut out = String::from("{");
    for (i, item) in st.fields.iter().enumerate() {
        if i > 0 {
            out.push_str(", ");
        }
        let _ = write!(out, "{}", format_value(item));
    }
    out.push('}');
    out
}

/// Drop an owned runtime value. Does not free FILE/MMAP handles (those have close).
/// Safe for overwrite paths (`list_set` / `dict_set` / `struct_set`).
pub(crate) fn free_rt_value(v: RtValue) {
    match v.kind {
        KIND_STR => {
            if v.payload != 0 {
                unsafe {
                    drop(CString::from_raw(v.payload as *mut c_char));
                }
            }
        }
        KIND_LIST => {
            if v.payload != 0 {
                let list = unsafe { Box::from_raw(v.payload as *mut RtList) };
                for item in list.items {
                    free_rt_value(item);
                }
            }
        }
        KIND_DICT => {
            if v.payload != 0 {
                let dict = unsafe { Box::from_raw(v.payload as *mut RtDict) };
                for (_k, item) in dict.entries {
                    free_rt_value(item);
                }
            }
        }
        KIND_STRUCT => {
            if v.payload != 0 {
                let st = unsafe { Box::from_raw(v.payload as *mut RtStruct) };
                for item in st.fields {
                    free_rt_value(item);
                }
            }
        }
        _ => {}
    }
}

fn key_to_string(key: i64, key_kind: i64) -> String {
    match key_kind {
        KIND_STR => {
            if key == 0 {
                String::new()
            } else {
                let cstr = unsafe { CStr::from_ptr(key as *const c_char) };
                cstr.to_str().unwrap_or("<?>").to_string()
            }
        }
        KIND_I64 => format!("{}", key),
        _ => format!("{}", key),
    }
}

/// Borrow key text for lookup without allocating when the key is already a C string.
fn key_as_str<'a>(key: i64, key_kind: i64, owned: &'a mut String) -> &'a str {
    match key_kind {
        KIND_STR => {
            if key == 0 {
                ""
            } else {
                let cstr = unsafe { CStr::from_ptr(key as *const c_char) };
                cstr.to_str().unwrap_or("<?>")
            }
        }
        _ => {
            *owned = key_to_string(key, key_kind);
            owned.as_str()
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_i64(v: i64) {
    print!("{}", v);
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_u64(v: i64) {
    print!("{}", v as u64);
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_f64(v: f64) {
    print!("{}", v);
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_str(s: *const i8) {
    if s.is_null() {
        return;
    }
    let cstr = unsafe { CStr::from_ptr(s) };
    match cstr.to_str() {
        Ok(text) => print!("{}", text),
        Err(_) => print!("<?>"),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_newline() {
    println!();
}

/// Separator emitted between `print` arguments (interpreter joins with a space).
#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_separator() {
    print!(" ");
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_pow_i64(base: i64, exp: i64) -> i64 {
    if exp < 0 {
        return 0;
    }
    let mut result: i64 = 1;
    let mut b = base;
    let mut e = exp as u64;
    while e > 0 {
        if e & 1 == 1 {
            result = result.wrapping_mul(b);
        }
        b = b.wrapping_mul(b);
        e >>= 1;
    }
    result
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_pow_f64(base: f64, exp: f64) -> f64 {
    base.powf(exp)
}

/// Seconds since the UNIX epoch as an `f64` bit pattern (`KIND_F64` payload).
#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_clock() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs_f64().to_bits() as i64)
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_floor_div_i64(a: i64, b: i64) -> i64 {
    a.div_euclid(b)
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_floor_div_f64(a: f64, b: f64) -> f64 {
    (a / b).floor()
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_list_new() -> i64 {
    let list = Box::new(RtList { items: Vec::new() });
    Box::into_raw(list) as i64
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_list_push(list: i64, value: i64, kind: i64) {
    if list == 0 {
        return;
    }
    let list = unsafe { &mut *(list as *mut RtList) };
    list.items.push(RtValue {
        kind,
        payload: value,
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_list(list: i64) {
    if list == 0 {
        print!("[]");
        return;
    }
    let list = unsafe { &*(list as *const RtList) };
    print!("{}", format_list(list));
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_dict_new() -> i64 {
    let dict = Box::new(RtDict {
        entries: IndexMap::new(),
    });
    Box::into_raw(dict) as i64
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_dict_push(dict: i64, key: i64, key_kind: i64, val: i64, val_kind: i64) {
    if dict == 0 {
        return;
    }
    let dict = unsafe { &mut *(dict as *mut RtDict) };
    let k = key_to_string(key, key_kind);
    let next = RtValue {
        kind: val_kind,
        payload: val,
    };
    if let Some(old) = dict.entries.insert(k, next) {
        free_rt_value(old);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_dict(dict: i64) {
    if dict == 0 {
        print!("{{}}");
        return;
    }
    let dict = unsafe { &*(dict as *const RtDict) };
    print!("{}", format_dict(dict));
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_value(payload: i64, kind: i64) {
    print!("{}", format_value(&RtValue { kind, payload }));
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_list_get(list: i64, index: i64, out_kind: *mut i64) -> i64 {
    if list == 0 || out_kind.is_null() {
        return 0;
    }
    let list = unsafe { &*(list as *const RtList) };
    if index < 0 || index as usize >= list.items.len() {
        unsafe {
            *out_kind = KIND_NONE;
        }
        return 0;
    }
    let item = &list.items[index as usize];
    unsafe {
        *out_kind = item.kind;
    }
    item.payload
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_list_set(list: i64, index: i64, value: i64, kind: i64) {
    if list == 0 {
        return;
    }
    let list = unsafe { &mut *(list as *mut RtList) };
    if index < 0 || index as usize >= list.items.len() {
        return;
    }
    let old = std::mem::replace(
        &mut list.items[index as usize],
        RtValue {
            kind,
            payload: value,
        },
    );
    free_rt_value(old);
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_dict_get(dict: i64, key: i64, key_kind: i64, out_kind: *mut i64) -> i64 {
    if dict == 0 || out_kind.is_null() {
        return 0;
    }
    let dict = unsafe { &*(dict as *const RtDict) };
    let mut owned = String::new();
    let k = key_as_str(key, key_kind, &mut owned);
    if let Some(ev) = dict.entries.get(k) {
        unsafe {
            *out_kind = ev.kind;
        }
        return ev.payload;
    }
    unsafe {
        *out_kind = KIND_NONE;
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_dict_set(dict: i64, key: i64, key_kind: i64, value: i64, val_kind: i64) {
    if dict == 0 {
        return;
    }
    let dict = unsafe { &mut *(dict as *mut RtDict) };
    let mut owned = String::new();
    let k = key_as_str(key, key_kind, &mut owned);
    if let Some(ev) = dict.entries.get_mut(k) {
        let old = std::mem::replace(
            ev,
            RtValue {
                kind: val_kind,
                payload: value,
            },
        );
        free_rt_value(old);
        return;
    }
    dict.entries.insert(
        dict_own_key(key, key_kind, owned),
        RtValue {
            kind: val_kind,
            payload: value,
        },
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_index_get(obj: i64, obj_kind: i64, idx: i64, idx_kind: i64, out_kind: *mut i64) -> i64 {
    if obj_kind == KIND_DICT {
        hyper_rt_dict_get(obj, idx, idx_kind, out_kind)
    } else if obj_kind == KIND_LIST {
        hyper_rt_list_get(obj, idx, out_kind)
    } else if !out_kind.is_null() {
        unsafe {
            *out_kind = KIND_NONE;
        }
        0
    } else {
        0
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_index_set(obj: i64, obj_kind: i64, idx: i64, idx_kind: i64, value: i64, val_kind: i64) {
    if obj_kind == KIND_DICT {
        hyper_rt_dict_set(obj, idx, idx_kind, value, val_kind);
    } else if obj_kind == KIND_LIST {
        hyper_rt_list_set(obj, idx, value, val_kind);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_list_len(list: i64) -> i64 {
    if list == 0 {
        return 0;
    }
    let list = unsafe { &*(list as *const RtList) };
    list.items.len() as i64
}

fn utf8_char_len(payload: i64) -> i64 {
    if payload == 0 {
        return 0;
    }
    let s = unsafe { CStr::from_ptr(payload as *const c_char) }
        .to_str()
        .unwrap_or("");
    s.chars().count() as i64
}

fn dict_len(dict: i64) -> i64 {
    if dict == 0 {
        return 0;
    }
    let dict = unsafe { &*(dict as *const RtDict) };
    dict.entries.len() as i64
}

/// `len()` on a list, array, dict, or string. Other kinds raise at `line`.
#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_coll_len(payload: i64, kind: i64, line: i64, _line_kind: i64) -> i64 {
    match kind {
        KIND_LIST => hyper_rt_list_len(payload),
        KIND_DICT => dict_len(payload),
        KIND_STR => utf8_char_len(payload),
        _ => error::runtime(line as u32, "this type has no method 'len'"),
    }
}

/// `append(x)` on a list or array. Mutates the handle in place.
#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_coll_append(payload: i64, kind: i64, value: i64, val_kind: i64, line: i64, _line_kind: i64) {
    if kind == KIND_LIST {
        hyper_rt_list_push(payload, value, val_kind);
        return;
    }
    if kind == KIND_DICT {
        error::runtime(line as u32, "dict has no method 'append'");
    }
    error::runtime(line as u32, "this type has no method 'append'");
}

/// `keys()` on a dict. Returns a new list of string keys in insertion order.
#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_coll_keys(payload: i64, kind: i64, line: i64, _line_kind: i64) -> i64 {
    if kind != KIND_DICT {
        if kind == KIND_LIST {
            error::runtime(line as u32, "list has no method 'keys'");
        }
        error::runtime(line as u32, "this type has no method 'keys'");
    }
    let list = hyper_rt_list_new();
    if payload == 0 {
        return list;
    }
    let dict = unsafe { &*(payload as *const RtDict) };
    for (key, _) in &dict.entries {
        let payload = match CString::new(key.as_str()) {
            Ok(c) => c.into_raw() as i64,
            Err(_) => 0,
        };
        hyper_rt_list_push(list, payload, KIND_STR);
    }
    list
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_value_to_str(payload: i64, kind: i64) -> i64 {
    let s = format_value(&RtValue { kind, payload });
    match CString::new(s) {
        Ok(c) => c.into_raw() as i64,
        Err(_) => 0,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_str_concat(left: i64, right: i64) -> i64 {
    let a = if left == 0 {
        String::new()
    } else {
        let cstr = unsafe { CStr::from_ptr(left as *const c_char) };
        cstr.to_str().unwrap_or("").to_string()
    };
    let b = if right == 0 {
        String::new()
    } else {
        let cstr = unsafe { CStr::from_ptr(right as *const c_char) };
        cstr.to_str().unwrap_or("").to_string()
    };
    match CString::new(format!("{}{}", a, b)) {
        Ok(c) => c.into_raw() as i64,
        Err(_) => 0,
    }
}

fn cstr_to_str<'a>(payload: i64) -> &'a str {
    if payload == 0 {
        return "";
    }
    unsafe { CStr::from_ptr(payload as *const c_char) }
        .to_str()
        .unwrap_or("")
}

/// Mirrors the interpreter's `==`: strings, lists and dicts compare by content,
/// numbers promote to f64 when either side is a float, and struct instances are
/// never equal to anything.
fn values_equal(a: &RtValue, b: &RtValue) -> bool {
    match (a.kind, b.kind) {
        (KIND_STR, KIND_STR) => cstr_to_str(a.payload) == cstr_to_str(b.payload),
        (KIND_NONE, KIND_NONE) => true,
        (KIND_BOOL, KIND_BOOL) => a.payload == b.payload,
        (KIND_F64, KIND_F64) => {
            f64::from_bits(a.payload as u64) == f64::from_bits(b.payload as u64)
        }
        (KIND_F64, KIND_I64) => f64::from_bits(a.payload as u64) == b.payload as f64,
        (KIND_I64, KIND_F64) => a.payload as f64 == f64::from_bits(b.payload as u64),
        (KIND_I64, KIND_I64) => a.payload == b.payload,
        (KIND_LIST, KIND_LIST) => {
            if a.payload == 0 || b.payload == 0 {
                return a.payload == b.payload;
            }
            let left = unsafe { &*(a.payload as *const RtList) };
            let right = unsafe { &*(b.payload as *const RtList) };
            left.items.len() == right.items.len()
                && left
                    .items
                    .iter()
                    .zip(right.items.iter())
                    .all(|(x, y)| values_equal(x, y))
        }
        (KIND_DICT, KIND_DICT) => {
            if a.payload == 0 || b.payload == 0 {
                return a.payload == b.payload;
            }
            let left = unsafe { &*(a.payload as *const RtDict) };
            let right = unsafe { &*(b.payload as *const RtDict) };
            left.entries.len() == right.entries.len()
                && left.entries.iter().all(|(key, value)| {
                    right
                        .entries
                        .get(key)
                        .is_some_and(|other| values_equal(value, other))
                })
        }
        _ => false,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_div_by_zero(line: i64) {
    error::runtime(line as u32, "division by zero");
}

thread_local! {
    static HANDLE_DEPTH: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
    static PENDING_RAISE: std::cell::RefCell<Option<String>> =
        const { std::cell::RefCell::new(None) };
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_handle_enter() -> i64 {
    HANDLE_DEPTH.with(|d| d.set(d.get() + 1));
    PENDING_RAISE.with(|cell| {
        *cell.borrow_mut() = None;
    });
    0
}

/// Returns 1 if a raise was recovered inside the matching handle region.
#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_handle_leave() -> i64 {
    HANDLE_DEPTH.with(|d| d.set(d.get().saturating_sub(1)));
    PENDING_RAISE.with(|cell| cell.borrow_mut().take().is_some() as i64)
}

/// Uncaught raise prints a RuntimeError and exits 70. Inside `handle`, records the
/// message and returns so the caller can take the fallback branch.
#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_raise(payload: i64, kind: i64, line: i64, _line_kind: i64) -> i64 {
    let msg = format_value(&RtValue { kind, payload });
    let depth = HANDLE_DEPTH.with(|d| d.get());
    if depth > 0 {
        PENDING_RAISE.with(|cell| {
            *cell.borrow_mut() = Some(msg);
        });
        0
    } else {
        error::runtime(line as u32, msg);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_value_eq(a: i64, a_kind: i64, b: i64, b_kind: i64) -> i64 {
    let left = RtValue {
        kind: a_kind,
        payload: a,
    };
    let right = RtValue {
        kind: b_kind,
        payload: b,
    };
    values_equal(&left, &right) as i64
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_struct_new(nfields: i64) -> i64 {
    let n = if nfields < 0 { 0 } else { nfields as usize };
    let st = Box::new(RtStruct {
        fields: vec![
            RtValue {
                kind: KIND_NONE,
                payload: 0,
            };
            n
        ],
    });
    Box::into_raw(st) as i64
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_struct_get(obj: i64, field: i64, out_kind: *mut i64) -> i64 {
    if obj == 0 || out_kind.is_null() {
        return 0;
    }
    let st = unsafe { &*(obj as *const RtStruct) };
    if field < 0 || field as usize >= st.fields.len() {
        unsafe {
            *out_kind = KIND_NONE;
        }
        return 0;
    }
    let item = &st.fields[field as usize];
    unsafe {
        *out_kind = item.kind;
    }
    item.payload
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_struct_set(obj: i64, field: i64, value: i64, kind: i64) {
    if obj == 0 {
        return;
    }
    let st = unsafe { &mut *(obj as *mut RtStruct) };
    if field < 0 || field as usize >= st.fields.len() {
        return;
    }
    let old = std::mem::replace(
        &mut st.fields[field as usize],
        RtValue {
            kind,
            payload: value,
        },
    );
    free_rt_value(old);
}

#[unsafe(no_mangle)]
pub extern "C" fn hyper_rt_print_struct(obj: i64) {
    if obj == 0 {
        print!("{{}}");
        return;
    }
    let st = unsafe { &*(obj as *const RtStruct) };
    print!("{}", format_struct(st));
}

#[cfg(test)]
mod tests {
    use super::*;

    fn str_payload(text: &str) -> i64 {
        CString::new(text).unwrap().into_raw() as i64
    }

    fn list_of(items: &[(i64, i64)]) -> i64 {
        let list = hyper_rt_list_new();
        for (payload, kind) in items {
            hyper_rt_list_push(list, *payload, *kind);
        }
        list
    }

    #[test]
    fn strings_compare_by_content() {
        let a = str_payload("hyper");
        let b = str_payload("hyper");
        let c = str_payload("rust");
        assert_ne!(a, b, "test needs two distinct allocations");
        assert_eq!(hyper_rt_value_eq(a, KIND_STR, b, KIND_STR), 1);
        assert_eq!(hyper_rt_value_eq(a, KIND_STR, c, KIND_STR), 0);
    }

    #[test]
    fn lists_compare_element_wise() {
        let a = list_of(&[(1, KIND_I64), (2, KIND_I64)]);
        let b = list_of(&[(1, KIND_I64), (2, KIND_I64)]);
        let c = list_of(&[(1, KIND_I64), (3, KIND_I64)]);
        let short = list_of(&[(1, KIND_I64)]);
        assert_eq!(hyper_rt_value_eq(a, KIND_LIST, b, KIND_LIST), 1);
        assert_eq!(hyper_rt_value_eq(a, KIND_LIST, c, KIND_LIST), 0);
        assert_eq!(hyper_rt_value_eq(a, KIND_LIST, short, KIND_LIST), 0);
    }

    #[test]
    fn nested_lists_compare_by_content() {
        let inner_a = list_of(&[(str_payload("x"), KIND_STR)]);
        let inner_b = list_of(&[(str_payload("x"), KIND_STR)]);
        let a = list_of(&[(inner_a, KIND_LIST)]);
        let b = list_of(&[(inner_b, KIND_LIST)]);
        assert_eq!(hyper_rt_value_eq(a, KIND_LIST, b, KIND_LIST), 1);
    }

    #[test]
    fn dicts_ignore_entry_order() {
        let a = hyper_rt_dict_new();
        hyper_rt_dict_push(a, str_payload("x"), KIND_STR, 1, KIND_I64);
        hyper_rt_dict_push(a, str_payload("y"), KIND_STR, 2, KIND_I64);
        let b = hyper_rt_dict_new();
        hyper_rt_dict_push(b, str_payload("y"), KIND_STR, 2, KIND_I64);
        hyper_rt_dict_push(b, str_payload("x"), KIND_STR, 1, KIND_I64);
        assert_eq!(hyper_rt_value_eq(a, KIND_DICT, b, KIND_DICT), 1);
    }

    #[test]
    fn coll_len_append_keys() {
        let list = list_of(&[(1, KIND_I64)]);
        assert_eq!(hyper_rt_coll_len(list, KIND_LIST, 1, 0), 1);
        hyper_rt_coll_append(list, KIND_LIST, 2, KIND_I64, 1, 0);
        assert_eq!(hyper_rt_coll_len(list, KIND_LIST, 1, 0), 2);

        let dict = hyper_rt_dict_new();
        hyper_rt_dict_push(dict, str_payload("math"), KIND_STR, 100, KIND_I64);
        hyper_rt_dict_push(dict, str_payload("physics"), KIND_STR, 95, KIND_I64);
        assert_eq!(hyper_rt_coll_len(dict, KIND_DICT, 1, 0), 2);
        let keys = hyper_rt_coll_keys(dict, KIND_DICT, 1, 0);
        assert_eq!(hyper_rt_coll_len(keys, KIND_LIST, 1, 0), 2);
        assert_eq!(hyper_rt_coll_len(str_payload("hi"), KIND_STR, 1, 0), 2);
    }

    #[test]
    fn integers_and_floats_compare_numerically() {
        let one = 1i64;
        let one_point_zero = 1.0f64.to_bits() as i64;
        assert_eq!(
            hyper_rt_value_eq(one, KIND_I64, one_point_zero, KIND_F64),
            1
        );
        assert_eq!(hyper_rt_value_eq(2, KIND_I64, one_point_zero, KIND_F64), 0);
    }

    #[test]
    fn booleans_do_not_equal_integers() {
        assert_eq!(hyper_rt_value_eq(1, KIND_BOOL, 1, KIND_BOOL), 1);
        assert_eq!(hyper_rt_value_eq(1, KIND_BOOL, 1, KIND_I64), 0);
    }

    #[test]
    fn struct_instances_are_never_equal() {
        let obj = hyper_rt_struct_new(1);
        assert_eq!(hyper_rt_value_eq(obj, KIND_STRUCT, obj, KIND_STRUCT), 0);
    }

    #[test]
    fn none_equals_none_only() {
        assert_eq!(hyper_rt_value_eq(0, KIND_NONE, 0, KIND_NONE), 1);
        assert_eq!(hyper_rt_value_eq(0, KIND_NONE, 0, KIND_I64), 0);
    }

    #[test]
    fn dict_get_set_overwrite_keeps_insertion_order() {
        let dict = hyper_rt_dict_new();
        hyper_rt_dict_push(dict, str_payload("a"), KIND_STR, 1, KIND_I64);
        hyper_rt_dict_push(dict, str_payload("b"), KIND_STR, 2, KIND_I64);
        hyper_rt_dict_set(dict, str_payload("a"), KIND_STR, 9, KIND_I64);
        let mut kind = KIND_NONE;
        assert_eq!(
            hyper_rt_dict_get(dict, str_payload("a"), KIND_STR, &mut kind),
            9
        );
        assert_eq!(kind, KIND_I64);
        let keys = hyper_rt_coll_keys(dict, KIND_DICT, 1, 0);
        let mut kkind = KIND_NONE;
        let first = hyper_rt_list_get(keys, 0, &mut kkind);
        assert_eq!(cstr_to_str(first), "a");
        let second = hyper_rt_list_get(keys, 1, &mut kkind);
        assert_eq!(cstr_to_str(second), "b");
        assert_eq!(
            hyper_rt_dict_get(dict, str_payload("missing"), KIND_STR, &mut kind),
            0
        );
        assert_eq!(kind, KIND_NONE);
    }

    /// Amortized O(1) get/set: 256 keys, then ~1e6 lookups on interned key pointers.
    /// Linear scan at this size is tens of times slower in debug builds.
    #[test]
    fn dict_get_set_on_medium_map() {
        const N: i64 = 256;
        let dict = hyper_rt_dict_new();
        let keys: Vec<i64> = (0..N).map(|i| str_payload(&format!("k{i}"))).collect();
        for (i, &key) in keys.iter().enumerate() {
            hyper_rt_dict_set(dict, key, KIND_STR, i as i64, KIND_I64);
        }
        assert_eq!(hyper_rt_coll_len(dict, KIND_DICT, 1, 0), N);

        let mut kind = KIND_NONE;
        let start = std::time::Instant::now();
        for _ in 0..4096 {
            for (i, &key) in keys.iter().enumerate() {
                assert_eq!(hyper_rt_dict_get(dict, key, KIND_STR, &mut kind), i as i64);
                assert_eq!(kind, KIND_I64);
            }
        }
        let elapsed = start.elapsed();
        hyper_rt_dict_set(dict, keys[0], KIND_STR, 999, KIND_I64);
        assert_eq!(hyper_rt_dict_get(dict, keys[0], KIND_STR, &mut kind), 999);
        assert!(
            elapsed.as_millis() < 1500,
            "medium dict get should be hash-backed, took {elapsed:?}"
        );
    }
}
