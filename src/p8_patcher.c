#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "p8_patcher.h"

#ifdef OS_FREERTOS
extern void *rh_malloc(size_t size);
extern void *rh_realloc(void *ptr, size_t size);
extern void rh_free(void *ptr);
#else
#define rh_malloc malloc
#define rh_realloc realloc
#define rh_free free
#endif

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

static void sb_init(StringBuilder *sb) {
    sb->capacity = 4096;
    sb->length = 0;
    sb->data = (char *)rh_malloc(sb->capacity);
    if (sb->data) sb->data[0] = '\0';
}

static void sb_append(StringBuilder *sb, const char *str, size_t len) {
    if (!sb->data) return;
    if (sb->length + len + 1 > sb->capacity) {
        sb->capacity = (sb->length + len + 1) * 2;
        sb->data = (char *)rh_realloc(sb->data, sb->capacity);
    }
    if (sb->data) {
        memcpy(sb->data + sb->length, str, len);
        sb->length += len;
        sb->data[sb->length] = '\0';
    }
}

// UTF-8の絵文字をP8SCIIの1バイトコードに置換する関数
static void replace_emoji_with_p8scii(char *str, const char *emoji, char p8scii_char) {
    char *pos = str;
    size_t len = strlen(emoji);
    while ((pos = strstr(pos, emoji)) != NULL) {
        *pos = p8scii_char;
        // 置き換えた1文字分以降の文字列を前に詰める
        memmove(pos + 1, pos + len, strlen(pos + len) + 1);
        pos++;
    }
}

// 空白・改行を無視して文字列の一致を探す関数
static const char *find_match(const char *haystack, const char *needle, size_t *out_len) {
    if (!haystack || !needle) return NULL;
    
    for (const char *h = haystack; *h; h++) {
        if (*h == ' ' || *h == '\t' || *h == '\r' || *h == '\n') continue;
        
        const char *h_ptr = h;
        const char *n_ptr = needle;
        
        while (*n_ptr) {
            while (*h_ptr == ' ' || *h_ptr == '\t' || *h_ptr == '\r' || *h_ptr == '\n') h_ptr++;
            while (*n_ptr == ' ' || *n_ptr == '\t' || *n_ptr == '\r' || *n_ptr == '\n') n_ptr++;
            
            if (!*n_ptr) break; // matched all of needle
            if (!*h_ptr) break; // end of haystack
            
            if (*h_ptr != *n_ptr) break; // mismatch
            
            h_ptr++;
            n_ptr++;
        }
        
        while (*n_ptr == ' ' || *n_ptr == '\t' || *n_ptr == '\r' || *n_ptr == '\n') n_ptr++;
        
        if (!*n_ptr) {
            // matched!
            if (out_len) *out_len = h_ptr - h;
            return h;
        }
    }
    return NULL;
}

char *apply_p8t_patch(const char *old_script, const char *p8t_path) {
    FILE *f = fopen(p8t_path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long p8t_len = ftell(f);
    rewind(f);

    if (p8t_len <= 0) {
        fclose(f);
        return NULL;
    }

    char *p8t_buf = (char *)rh_malloc(p8t_len + 1);
    fread(p8t_buf, 1, p8t_len, f);
    p8t_buf[p8t_len] = '\0';
    fclose(f);

    // ==========================================
    // UTF-8絵文字をバイトコードで直接指定して置換
    // （異体字セレクタ \xEF\xB8\x8F の有無両方に対応）
    // ==========================================
    replace_emoji_with_p8scii(p8t_buf, "\xE2\xAC\x85\xEF\xB8\x8F", '\x8b'); // ⬅️
    replace_emoji_with_p8scii(p8t_buf, "\xE2\xAC\x85", '\x8b');             // ⬅
    replace_emoji_with_p8scii(p8t_buf, "\xE2\x9E\xA1\xEF\xB8\x8F", '\x91'); // ➡️
    replace_emoji_with_p8scii(p8t_buf, "\xE2\x9E\xA1", '\x91');             // ➡
    replace_emoji_with_p8scii(p8t_buf, "\xE2\xAC\x86\xEF\xB8\x8F", '\x94'); // ⬆️
    replace_emoji_with_p8scii(p8t_buf, "\xE2\xAC\x86", '\x94');             // ⬆
    replace_emoji_with_p8scii(p8t_buf, "\xE2\xAC\x87\xEF\xB8\x8F", '\x83'); // ⬇️
    replace_emoji_with_p8scii(p8t_buf, "\xE2\xAC\x87", '\x83');             // ⬇
    replace_emoji_with_p8scii(p8t_buf, "\xF0\x9F\x85\xBE\xEF\xB8\x8F", '\x8e'); // 🅾️
    replace_emoji_with_p8scii(p8t_buf, "\xF0\x9F\x85\xBE", '\x8e');             // 🅾
    replace_emoji_with_p8scii(p8t_buf, "\xE2\x9D\x8E\xEF\xB8\x8F", '\x97'); // ❎
    replace_emoji_with_p8scii(p8t_buf, "\xE2\x9D\x8E", '\x97');             // ❎
    // ==========================================

    // BOM (Byte Order Mark) がある場合はスキップする
    char *start_ptr = p8t_buf;
    if (p8t_len >= 3 && (unsigned char)p8t_buf[0] == 0xEF && (unsigned char)p8t_buf[1] == 0xBB && (unsigned char)p8t_buf[2] == 0xBF) {
        start_ptr += 3;
    }

    // Make a mutable copy of old_script to work with
    size_t old_len = strlen(old_script);
    char *script_buf = (char *)rh_malloc(old_len + 1);
    memcpy(script_buf, old_script, old_len + 1);

    // Apply patches sequentially
    char *current_script = script_buf;

    char *line = start_ptr;
    while (line && *line) {
        char *next_line = strchr(line, '\n');
        if (next_line) *next_line++ = '\0';

        if (strncmp(line, "[SEARCH]", 8) == 0) {
            StringBuilder search_sb;
            StringBuilder replace_sb;
            sb_init(&search_sb);
            sb_init(&replace_sb);

            int state = 0; // 0=search, 1=replace
            bool block_closed = false;

            while (next_line && *next_line) {
                line = next_line;
                next_line = strchr(line, '\n');
                if (next_line) *next_line++ = '\0';

                if (strncmp(line, "[REPLACE]", 9) == 0) {
                    state = 1;
                    continue;
                } else if (strncmp(line, "[\\]", 3) == 0 || strncmp(line, "[/]", 3) == 0) {
                    block_closed = true;
                    break;
                }

                if (state == 0) {
                    sb_append(&search_sb, line, strlen(line));
                    sb_append(&search_sb, "\n", 1);
                } else {
                    sb_append(&replace_sb, line, strlen(line));
                    sb_append(&replace_sb, "\n", 1);
                }
            }

            if (block_closed && search_sb.length > 0) {
                size_t match_len = 0;
                const char *match = find_match(current_script, search_sb.data, &match_len);
                if (match) {
                    StringBuilder new_script;
                    sb_init(&new_script);

                    // Append prefix
                    sb_append(&new_script, current_script, match - current_script);
                    // Append replacement
                    sb_append(&new_script, replace_sb.data, replace_sb.length);
                    // Append suffix
                    const char *suffix = match + match_len;
                    sb_append(&new_script, suffix, strlen(suffix));

                    rh_free(current_script);
                    current_script = new_script.data;
                } else {
                    printf("Patch warning: Could not find SEARCH block:\n%s\n", search_sb.data);
                }
            }
            rh_free(search_sb.data);
            rh_free(replace_sb.data);
            line = next_line;
        } else {
            line = next_line;
        }
    }

    rh_free(p8t_buf);
    return current_script;
}