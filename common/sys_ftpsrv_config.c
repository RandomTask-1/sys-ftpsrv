// Copyright (c) 2026 RandomTask
// SPDX-License-Identifier: MIT
#include "sys_ftpsrv_config.h"
#include "cfgini.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

// one bit per option, so that the first occurrence of a key wins (as with
// minIni's getters) even if a hand edited file repeats it.
enum {
    Field_User,
    Field_Pass,
    Field_Port,
    Field_Timeout,
    Field_UserPresets,
    Field_PassLength,
#define SYS_FTPSRV_X_ENUM(field, section, key, def) Field_##field,
    SYS_FTPSRV_BOOL_OPTIONS(SYS_FTPSRV_X_ENUM)
#undef SYS_FTPSRV_X_ENUM
    Field_Count,
};

_Static_assert(Field_Count <= 64, "too many config fields for the seen mask");

struct LoadContext {
    struct SysFtpConfig* cfg;
    u64 seen;
};

static bool matches(const char* section, const char* key, const char* want_section, const char* want_key) {
    return !strcasecmp(section, want_section) && !strcasecmp(key, want_key);
}

// returns true if the field hasn't been seen yet, and marks it as seen.
static bool first(struct LoadContext* ctx, int field) {
    const u64 bit = 1ULL << field;
    if (ctx->seen & bit) {
        return false;
    }
    ctx->seen |= bit;
    return true;
}

static bool load_callback(const char* section, const char* key, const char* value, void* user) {
    struct LoadContext* ctx = user;
    struct SysFtpConfig* cfg = ctx->cfg;

    if (matches(section, key, SYS_FTPSRV_SECTION_LOGIN, SYS_FTPSRV_KEY_USER)) {
        if (first(ctx, Field_User)) {
            snprintf(cfg->user, sizeof(cfg->user), "%s", value);
        }
    } else if (matches(section, key, SYS_FTPSRV_SECTION_LOGIN, SYS_FTPSRV_KEY_PASS)) {
        if (first(ctx, Field_Pass)) {
            snprintf(cfg->pass, sizeof(cfg->pass), "%s", value);
        }
    } else if (matches(section, key, SYS_FTPSRV_SECTION_NETWORK, SYS_FTPSRV_KEY_PORT)) {
        if (first(ctx, Field_Port)) {
            cfg->port = cfgini_parse_long(value, cfg->port);
        }
    } else if (matches(section, key, SYS_FTPSRV_SECTION_NETWORK, SYS_FTPSRV_KEY_TIMEOUT)) {
        if (first(ctx, Field_Timeout)) {
            cfg->timeout = cfgini_parse_long(value, cfg->timeout);
        }
    } else if (matches(section, key, SYS_FTPSRV_SECTION_PRESETS, SYS_FTPSRV_KEY_USERS)) {
        if (first(ctx, Field_UserPresets)) {
            snprintf(cfg->user_presets, sizeof(cfg->user_presets), "%s", value);
        }
    } else if (matches(section, key, SYS_FTPSRV_SECTION_PASSWORD, SYS_FTPSRV_KEY_PASS_LENGTH)) {
        if (first(ctx, Field_PassLength)) {
            cfg->pass_length = cfgini_parse_long(value, cfg->pass_length);
        }
    }
#define SYS_FTPSRV_X_LOAD(field, section_name, key_name, def) \
    else if (matches(section, key, section_name, key_name)) { \
        if (first(ctx, Field_##field)) { \
            cfg->field = cfgini_parse_bool(value, cfg->field); \
        } \
    }
    SYS_FTPSRV_BOOL_OPTIONS(SYS_FTPSRV_X_LOAD)
#undef SYS_FTPSRV_X_LOAD

    return true;
}

void sys_ftpsrv_config_load(struct SysFtpConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    // defaults, used for anything missing from the file (or a missing file).
    cfg->port = SYS_FTPSRV_DEFAULT_PORT;
    cfg->timeout = SYS_FTPSRV_DEFAULT_TIMEOUT;
    snprintf(cfg->user_presets, sizeof(cfg->user_presets), "%s", SYS_FTPSRV_DEFAULT_USERS);
    cfg->pass_length = SYS_FTPSRV_DEFAULT_PASS_LEN;
#define SYS_FTPSRV_X_DEFAULT(field, section, key, def) cfg->field = def;
    SYS_FTPSRV_BOOL_OPTIONS(SYS_FTPSRV_X_DEFAULT)
#undef SYS_FTPSRV_X_DEFAULT

    struct LoadContext ctx = { cfg, 0 };
    cfgini_browse(SYS_FTPSRV_CONFIG_PATH, load_callback, &ctx);

    if (cfg->port <= 0 || cfg->port > 65535) {
        cfg->port = SYS_FTPSRV_DEFAULT_PORT;
    }

    if (cfg->timeout < 0) {
        cfg->timeout = SYS_FTPSRV_DEFAULT_TIMEOUT;
    }

    if (cfg->pass_length < SYS_FTPSRV_MIN_PASS_LEN || cfg->pass_length > SYS_FTPSRV_MAX_PASS_LEN) {
        cfg->pass_length = SYS_FTPSRV_DEFAULT_PASS_LEN;
    }
}

bool sys_ftpsrv_config_has_login(const struct SysFtpConfig* cfg) {
    return cfg->anon || cfg->user[0] || cfg->pass[0];
}

bool sys_ftpsrv_config_has_mounts(const struct SysFtpConfig* cfg) {
    return cfg->mount_album || cfg->mount_switch || cfg->mount_ams_contents ||
        cfg->mount_saves || cfg->mount_gamecard || cfg->mount_content_system ||
        cfg->mount_content_user || cfg->mount_content_sd || cfg->mount_bis_fs ||
        cfg->mount_bis_storage;
}

void sys_ftpsrv_config_create_dir(void) {
    FsFileSystem fs;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&fs))) {
        // both fail harmlessly if the folder already exists.
        fsFsCreateDirectory(&fs, "/config");
        fsFsCreateDirectory(&fs, SYS_FTPSRV_CONFIG_DIR);
        fsFsClose(&fs);
    }
}

static bool sd_file_exists(FsFileSystem* fs, const char* path) {
    FsDirEntryType type;
    return R_SUCCEEDED(fsFsGetEntryType(fs, path, &type)) && type == FsDirEntryType_File;
}

struct SphairaContext {
    struct SphairaFtpInfo* out;
    bool have_enabled;
    bool have_port;
};

static bool sphaira_callback(const char* section, const char* key, const char* value, void* user) {
    struct SphairaContext* ctx = user;

    // keys and defaults match sphaira's app.hpp (m_ftp_enabled / m_ftp_port).
    if (!ctx->have_enabled && matches(section, key, "config", "ftp_enabled")) {
        ctx->out->enabled = cfgini_parse_bool(value, false);
        ctx->have_enabled = true;
    } else if (!ctx->have_port && matches(section, key, "ftp", "port")) {
        ctx->out->port = cfgini_parse_long(value, SPHAIRA_DEFAULT_FTP_PORT);
        ctx->have_port = true;
    }

    // stop once both are found.
    return !(ctx->have_enabled && ctx->have_port);
}

void sys_ftpsrv_get_sphaira_ftp(struct SphairaFtpInfo* out) {
    memset(out, 0, sizeof(*out));
    out->port = SPHAIRA_DEFAULT_FTP_PORT;

    FsFileSystem fs;
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) {
        return;
    }
    out->installed = sd_file_exists(&fs, SPHAIRA_CONFIG_PATH) || sd_file_exists(&fs, SPHAIRA_NRO_PATH);
    fsFsClose(&fs);

    if (!out->installed) {
        return;
    }

    struct SphairaContext ctx = { out, false, false };
    cfgini_browse(SPHAIRA_CONFIG_PATH, sphaira_callback, &ctx);

    // sphaira falls back to 5000 when no port is set.
    if (out->port <= 0) {
        out->port = SPHAIRA_DEFAULT_FTP_PORT;
    }
}

bool sys_ftpsrv_config_set_bool(const char* section, const char* key, bool value) {
    // written as 1 / 0 as every ini_getbool() style parser accepts those.
    return sys_ftpsrv_config_set_string(section, key, value ? "1" : "0");
}

bool sys_ftpsrv_config_set_long(const char* section, const char* key, long value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", value);
    return sys_ftpsrv_config_set_string(section, key, buf);
}

bool sys_ftpsrv_config_set_string(const char* section, const char* key, const char* value) {
    return cfgini_set(SYS_FTPSRV_CONFIG_PATH, section, key, value);
}
