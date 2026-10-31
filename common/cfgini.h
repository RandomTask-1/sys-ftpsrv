// Copyright (c) 2026 RandomTask
// SPDX-License-Identifier: MIT
//
// minimal ini reader / writer used by both the sysmodule and the overlay.
// it understands the same syntax as minIni (which sphaira writes its config
// with) but uses no heap, and edits a file in place: only the value being set
// changes, every other byte (comments, spacing, line endings) is kept.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// longest key / section name and value that are parsed, longer ones are
// truncated (but still copied through unchanged when the file is edited).
#define CFGINI_MAX_NAME  64
#define CFGINI_MAX_VALUE 768

// called for every key, in file order. return false to stop browsing.
typedef bool (*CfgIniCallback)(const char* section, const char* key, const char* value, void* user);

// paths are absolute sd card paths, e.g. "/config/sys-ftpsrv/config.ini".
// returns false if the file can't be opened.
bool cfgini_browse(const char* path, CfgIniCallback callback, void* user);

// sets section.key = value, quoting the value when needed. a missing key is
// added after the last key of its section and a missing section is appended,
// the file is created if it doesn't exist (its folder must exist).
bool cfgini_set(const char* path, const char* section, const char* key, const char* value);

// parse values the same way minIni's ini_getbool() / ini_getl() do.
bool cfgini_parse_bool(const char* value, bool def);
long cfgini_parse_long(const char* value, long def);

#ifdef __cplusplus
}
#endif
