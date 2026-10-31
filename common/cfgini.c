// Copyright (c) 2026 RandomTask
// SPDX-License-Identifier: MIT
#include "cfgini.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// file io: libnx fs on the switch (the sysmodule has no heap and no stdio sd
// mount), stdio elsewhere so that the parser can be tested on a pc.
// ---------------------------------------------------------------------------

#ifdef __SWITCH__
#include <switch.h>

typedef struct {
    FsFileSystem fs;
    FsFile file;
} IoFile;

static bool io_open(IoFile* f, const char* path, bool write) {
    if (R_FAILED(fsOpenSdCardFileSystem(&f->fs))) {
        return false;
    }

    if (write) {
        fsFsDeleteFile(&f->fs, path);
        if (R_FAILED(fsFsCreateFile(&f->fs, path, 0, 0))) {
            fsFsClose(&f->fs);
            return false;
        }
    }

    const u32 mode = write ? (FsOpenMode_Write | FsOpenMode_Append) : FsOpenMode_Read;
    if (R_FAILED(fsFsOpenFile(&f->fs, path, mode, &f->file))) {
        fsFsClose(&f->fs);
        return false;
    }
    return true;
}

static long io_read_at(IoFile* f, long long offset, void* buf, unsigned long size) {
    u64 bytes_read = 0;
    if (R_FAILED(fsFileRead(&f->file, offset, buf, size, FsReadOption_None, &bytes_read))) {
        return -1;
    }
    return (long)bytes_read;
}

static bool io_write_at(IoFile* f, long long offset, const void* buf, unsigned long size) {
    return R_SUCCEEDED(fsFileWrite(&f->file, offset, buf, size, FsWriteOption_None));
}

static void io_close(IoFile* f) {
    fsFileClose(&f->file);
    fsFsClose(&f->fs);
}

static bool io_replace(const char* tmp_path, const char* path) {
    FsFileSystem fs;
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) {
        return false;
    }
    // fs can't rename over an existing file.
    fsFsDeleteFile(&fs, path);
    const bool ok = R_SUCCEEDED(fsFsRenameFile(&fs, tmp_path, path));
    fsFsClose(&fs);
    return ok;
}

static void io_delete(const char* path) {
    FsFileSystem fs;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&fs))) {
        fsFsDeleteFile(&fs, path);
        fsFsClose(&fs);
    }
}
#else
#include <stdio.h>

typedef struct {
    FILE* fp;
} IoFile;

static bool io_open(IoFile* f, const char* path, bool write) {
    f->fp = fopen(path, write ? "wb" : "rb");
    return f->fp != NULL;
}

static long io_read_at(IoFile* f, long long offset, void* buf, unsigned long size) {
    if (fseek(f->fp, (long)offset, SEEK_SET)) {
        return -1;
    }
    return (long)fread(buf, 1, size, f->fp);
}

static bool io_write_at(IoFile* f, long long offset, const void* buf, unsigned long size) {
    return !fseek(f->fp, (long)offset, SEEK_SET) && fwrite(buf, 1, size, f->fp) == size;
}

static void io_close(IoFile* f) {
    fclose(f->fp);
}

static bool io_replace(const char* tmp_path, const char* path) {
    remove(path);
    return !rename(tmp_path, path);
}

static void io_delete(const char* path) {
    remove(path);
}
#endif

// ---------------------------------------------------------------------------
// line reader, tracks the file offsets of each line so that edits can copy
// the original bytes around the part that changes.
// ---------------------------------------------------------------------------

typedef struct {
    IoFile* file;
    char buf[512];
    long long buf_offset;   // file offset of buf[0]
    long len;
    long pos;
    bool eof;
} Reader;

typedef struct {
    // line content without its line ending, truncated if it's too long.
    char text[CFGINI_MAX_NAME + CFGINI_MAX_VALUE + 64];
    long long start;        // offset of the first byte of the line
    long long content_end;  // offset just after the content (before \r\n)
    long long end;          // offset just after the line ending
    bool has_newline;
} Line;

static void reader_init(Reader* r, IoFile* file) {
    memset(r, 0, sizeof(*r));
    r->file = file;
}

static int reader_getc(Reader* r) {
    if (r->pos == r->len) {
        if (r->eof) {
            return -1;
        }

        r->buf_offset += r->len;
        r->len = io_read_at(r->file, r->buf_offset, r->buf, sizeof(r->buf));
        r->pos = 0;
        if (r->len <= 0) {
            r->len = 0;
            r->eof = true;
            return -1;
        }
    }
    return (unsigned char)r->buf[r->pos++];
}

static long long reader_offset(const Reader* r) {
    return r->buf_offset + r->pos;
}

static bool reader_next(Reader* r, Line* line) {
    line->start = reader_offset(r);
    line->has_newline = false;

    unsigned long text_len = 0;
    bool any = false;
    bool last_was_cr = false;
    int c;

    while ((c = reader_getc(r)) >= 0) {
        any = true;
        if (c == '\n') {
            line->has_newline = true;
            break;
        }

        last_was_cr = c == '\r';
        if (text_len < sizeof(line->text) - 1) {
            line->text[text_len++] = (char)c;
        }
    }

    if (!any) {
        return false;
    }

    line->end = reader_offset(r);
    line->content_end = line->has_newline ? line->end - 1 : line->end;

    // a "\r\n" line ending isn't part of the content.
    if (line->has_newline && last_was_cr) {
        line->content_end--;
        if (text_len && line->text[text_len - 1] == '\r') {
            text_len--;
        }
    }

    line->text[text_len] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

typedef enum {
    LineType_Other,     // blank, comment or not understood
    LineType_Section,
    LineType_Key,
} LineType;

typedef struct {
    LineType type;
    char name[CFGINI_MAX_NAME];     // section or key name
    char value[CFGINI_MAX_VALUE];
    unsigned long value_index;      // index of the raw value in Line.text
} Parsed;

static bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

static void copy_trimmed(char* out, unsigned long out_size, const char* begin, const char* end) {
    while (begin < end && is_blank(*begin)) {
        begin++;
    }
    while (end > begin && is_blank(end[-1])) {
        end--;
    }

    unsigned long len = (unsigned long)(end - begin);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, begin, len);
    out[len] = '\0';
}

// same rules as minIni: a value in double quotes is taken literally (with \"
// for a quote), otherwise it ends at a ';' or '#' comment and is trimmed.
static void parse_value(const char* raw, char* out, unsigned long out_size) {
    if (*raw == '"') {
        unsigned long len = 0;
        for (const char* p = raw + 1; *p && *p != '"'; p++) {
            if (p[0] == '\\' && p[1] == '"') {
                p++;
            }
            if (len < out_size - 1) {
                out[len++] = *p;
            }
        }
        out[len] = '\0';
        return;
    }

    const char* end = raw;
    while (*end && *end != ';' && *end != '#') {
        end++;
    }
    copy_trimmed(out, out_size, raw, end);
}

static void parse_line(const char* text, Parsed* out) {
    out->type = LineType_Other;

    const char* p = text;
    while (is_blank(*p)) {
        p++;
    }

    if (!*p || *p == ';' || *p == '#') {
        return;
    }

    if (*p == '[') {
        const char* close = strchr(p, ']');
        if (close) {
            copy_trimmed(out->name, sizeof(out->name), p + 1, close);
            out->type = LineType_Section;
        }
        return;
    }

    // minIni accepts ':' as the separator when there is no '='.
    const char* sep = strchr(p, '=');
    if (!sep) {
        sep = strchr(p, ':');
    }
    if (!sep) {
        return;
    }

    copy_trimmed(out->name, sizeof(out->name), p, sep);
    if (!out->name[0]) {
        return;
    }

    const char* value = sep + 1;
    while (is_blank(*value)) {
        value++;
    }

    out->value_index = (unsigned long)(value - text);
    parse_value(value, out->value, sizeof(out->value));
    out->type = LineType_Key;
}

// ---------------------------------------------------------------------------
// public api
// ---------------------------------------------------------------------------

bool cfgini_browse(const char* path, CfgIniCallback callback, void* user) {
    IoFile file;
    if (!io_open(&file, path, false)) {
        return false;
    }

    Reader reader;
    Line line;
    Parsed parsed;
    char section[CFGINI_MAX_NAME] = "";

    reader_init(&reader, &file);
    while (reader_next(&reader, &line)) {
        parse_line(line.text, &parsed);
        if (parsed.type == LineType_Section) {
            memcpy(section, parsed.name, sizeof(section));
        } else if (parsed.type == LineType_Key) {
            if (!callback(section, parsed.name, parsed.value, user)) {
                break;
            }
        }
    }

    io_close(&file);
    return true;
}

// quotes the value if it would otherwise be read back differently.
static void encode_value(const char* value, char* out, unsigned long out_size) {
    const unsigned long len = strlen(value);
    const bool needs_quotes = !len || strpbrk(value, "\";#") ||
        is_blank(value[0]) || is_blank(value[len - 1]);

    if (!needs_quotes) {
        strncpy(out, value, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    unsigned long n = 0;
    out[n++] = '"';
    for (const char* p = value; *p && n < out_size - 3; p++) {
        if (*p == '"') {
            out[n++] = '\\';
        }
        out[n++] = *p;
    }
    out[n++] = '"';
    out[n] = '\0';
}

typedef struct {
    IoFile* src;
    IoFile* dst;
    long long dst_offset;
    bool ok;
} Copier;

static void copier_write(Copier* c, const char* data, unsigned long size) {
    if (c->ok && size) {
        c->ok = io_write_at(c->dst, c->dst_offset, data, size);
        c->dst_offset += size;
    }
}

static void copier_write_str(Copier* c, const char* str) {
    copier_write(c, str, strlen(str));
}

// copies src[from, to), to < 0 means until the end of the file.
static void copier_copy(Copier* c, long long from, long long to) {
    char buf[512];
    while (c->ok && (to < 0 || from < to)) {
        unsigned long chunk = sizeof(buf);
        if (to >= 0 && (long long)chunk > to - from) {
            chunk = (unsigned long)(to - from);
        }

        const long n = io_read_at(c->src, from, buf, chunk);
        if (n < 0) {
            c->ok = false;
        }
        if (n <= 0) {
            break;
        }
        copier_write(c, buf, (unsigned long)n);
        from += n;
    }
}

bool cfgini_set(const char* path, const char* section, const char* key, const char* value) {
    char encoded[CFGINI_MAX_VALUE + 8];
    encode_value(value, encoded, sizeof(encoded));

    // pass 1: find where the value goes.
    long long replace_start = -1;
    long long replace_end = -1;
    long long insert_at = -1;
    bool insert_needs_newline = false;
    bool section_found = false;
    bool file_ends_with_newline = true;
    long long file_size = 0;

    IoFile src;
    const bool src_exists = io_open(&src, path, false);
    if (src_exists) {
        Reader reader;
        Line line;
        Parsed parsed;
        bool in_section = false;

        reader_init(&reader, &src);
        while (reader_next(&reader, &line)) {
            file_size = line.end;
            file_ends_with_newline = line.has_newline;

            parse_line(line.text, &parsed);
            if (parsed.type == LineType_Section) {
                // only the first matching section is used, as minIni does.
                in_section = !section_found && !strcasecmp(parsed.name, section);
                if (in_section) {
                    section_found = true;
                    insert_at = line.end;
                    insert_needs_newline = !line.has_newline;
                }
            } else if (parsed.type == LineType_Key && in_section) {
                if (replace_start < 0 && !strcasecmp(parsed.name, key)) {
                    replace_start = line.start + (long long)parsed.value_index;
                    replace_end = line.content_end;
                }
                insert_at = line.end;
                insert_needs_newline = !line.has_newline;
            }
        }
    }

    // pass 2: write the edited copy next to the original, then swap them.
    char tmp_path[256];
    if (strlen(path) + 5 > sizeof(tmp_path)) {
        if (src_exists) {
            io_close(&src);
        }
        return false;
    }
    strcpy(tmp_path, path);
    strcat(tmp_path, ".tmp");

    IoFile dst;
    if (!io_open(&dst, tmp_path, true)) {
        if (src_exists) {
            io_close(&src);
        }
        return false;
    }

    Copier c = { src_exists ? &src : NULL, &dst, 0, true };

    if (replace_start >= 0) {
        copier_copy(&c, 0, replace_start);
        copier_write_str(&c, encoded);
        copier_copy(&c, replace_end, -1);
    } else if (section_found) {
        copier_copy(&c, 0, insert_at);
        if (insert_needs_newline) {
            copier_write_str(&c, "\n");
        }
        copier_write_str(&c, key);
        copier_write_str(&c, " = ");
        copier_write_str(&c, encoded);
        copier_write_str(&c, "\n");
        copier_copy(&c, insert_at, -1);
    } else {
        if (src_exists) {
            copier_copy(&c, 0, -1);
            if (file_size) {
                copier_write_str(&c, file_ends_with_newline ? "\n" : "\n\n");
            }
        }
        copier_write_str(&c, "[");
        copier_write_str(&c, section);
        copier_write_str(&c, "]\n");
        copier_write_str(&c, key);
        copier_write_str(&c, " = ");
        copier_write_str(&c, encoded);
        copier_write_str(&c, "\n");
    }

    io_close(&dst);
    if (src_exists) {
        io_close(&src);
    }

    if (!c.ok) {
        io_delete(tmp_path);
        return false;
    }
    return io_replace(tmp_path, path);
}

bool cfgini_parse_bool(const char* value, bool def) {
    switch (toupper((unsigned char)value[0])) {
        case 'Y': case 'T': case '1':
            return true;
        case 'N': case 'F': case '0':
            return false;
        default:
            return def;
    }
}

long cfgini_parse_long(const char* value, long def) {
    if (!value[0]) {
        return def;
    }
    // minIni treats "0x..." as hex.
    const int base = (value[0] && toupper((unsigned char)value[1]) == 'X') ? 16 : 10;
    return strtol(value, NULL, base);
}
