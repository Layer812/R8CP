#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "p8_patcher.h"
#include <errno.h>
#include "r8cp_terra_r2_embedded.h"
#ifdef IS_CARDPUTER
extern long r8cp_p8t_sd_size(const char *posix_path);
extern long r8cp_p8t_sd_read(const char *posix_path, char *dest, size_t length);
#endif

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
    long p8t_len = 0;
    char *p8t_buf = NULL;
    FILE *f = fopen(p8t_path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            p8t_len = ftell(f);
            rewind(f);
            if (p8t_len > 0 && p8t_len <= 1048576) {
                p8t_buf = (char *)rh_malloc((size_t)p8t_len + 1);
                if (p8t_buf && fread(p8t_buf, 1, (size_t)p8t_len, f) != (size_t)p8t_len) {
                    rh_free(p8t_buf);
                    p8t_buf = NULL;
                }
            }
        }
        fclose(f);
    }
#ifdef IS_CARDPUTER
    if (!p8t_buf) {
        const long sd_len = r8cp_p8t_sd_size(p8t_path);
        if (sd_len > 0 && sd_len <= 1048576) {
            char *sd_buf = (char *)rh_malloc((size_t)sd_len + 1);
            if (sd_buf) {
                if (r8cp_p8t_sd_read(p8t_path, sd_buf, (size_t)sd_len) == sd_len) {
                    p8t_buf = sd_buf;
                    p8t_len = sd_len;
                } else {
                    rh_free(sd_buf);
                }
            }
        }
    }
#endif
    if (p8t_buf) p8t_buf[p8t_len] = '\0';
    const char *patch_name = strrchr(p8t_path, '/');
    patch_name = patch_name ? patch_name + 1 : p8t_path;
    if (strcmp(patch_name, "terra_1cart-44.p8t") == 0) {
        const bool old_patch = p8t_buf &&
            strstr(p8t_buf, "54k no-op calls removed") &&
            !strstr(p8t_buf, "r8_terra_music_toggle");
        if (!p8t_buf || old_patch) {
            const size_t new_len = sizeof(r8cp_terra_r2_embedded_p8t) - 1;
            char *new_buf = (char *)rh_malloc(new_len + 1);
            if (new_buf) {
                memcpy(new_buf, r8cp_terra_r2_embedded_p8t, new_len + 1);
                if (p8t_buf) rh_free(p8t_buf);
                p8t_buf = new_buf;
                p8t_len = (long)new_len;
            }
        }
    }
    if (!p8t_buf) return NULL;
    p8t_buf[p8t_len] = '\0';

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

    size_t old_len = strlen(old_script);
    if (p8t_len <= 0 || (size_t)p8t_len > (size_t)-1 - old_len - 1) {
        rh_free(p8t_buf);
        return NULL;
    }
    const size_t script_capacity = old_len + (size_t)p8t_len + 1;
    char *current_script = (char *)rh_malloc(script_capacity);
    if (!current_script) {
        rh_free(p8t_buf);
        return NULL;
    }
    memcpy(current_script, old_script, old_len + 1);
    size_t current_length = old_len;
    unsigned matched_blocks = 0;
    unsigned total_blocks = 0;

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
                ++total_blocks;
                size_t match_len = 0;
                const char *match = find_match(current_script, search_sb.data, &match_len);
                if (match) {
                    const size_t pos = (size_t)(match - current_script);
                    const size_t added = replace_sb.length;
                    if (pos > current_length || match_len > current_length - pos ||
                        added > script_capacity - 1 - (current_length - match_len)) {
                        rh_free(search_sb.data);
                        rh_free(replace_sb.data);
                        rh_free(p8t_buf);
                        rh_free(current_script);
                        return NULL;
                    }
                    const size_t tail = current_length - pos - match_len;
                    memmove(current_script + pos + added,
                            current_script + pos + match_len, tail + 1);
                    memcpy(current_script + pos, replace_sb.data, added);
                    current_length = current_length - match_len + added;
                    ++matched_blocks;
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
    if (strcmp(patch_name, "terra_1cart-44.p8t") == 0 &&
        (total_blocks != 4 || matched_blocks != 4)) {
        rh_free(current_script);
        return NULL;
    }
    return current_script;
}