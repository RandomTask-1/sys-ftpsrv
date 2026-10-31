// Copyright (c) 2026 RandomTask
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ultrahand (libultrahand) overlay for sys-ftpsrv. GPL as it is built with
// libultrahand, see LICENSE.
// starts / stops the sysmodule via pm, toggles boot2 and edits the shared
// config.ini. settings are read by the sysmodule on start, so changes made
// whilst the server is running are applied by restarting it.
#define TESLA_INIT_IMPL
#if USE_EXCEPTION_WRAP
#include <exception_wrap.hpp>
#endif
#include <tesla.hpp>

#include "sys_ftpsrv_config.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

#define STR2(x) #x
#define STR(x) STR2(x)

constexpr u64 PROGRAM_ID = SYS_FTPSRV_PROGRAM_ID;
constexpr const char* CONTENTS_DIR = "/atmosphere/contents/" SYS_FTPSRV_PROGRAM_ID_STR;
constexpr const char* EXEFS_PATH = "/atmosphere/contents/" SYS_FTPSRV_PROGRAM_ID_STR "/exefs.nsp";
constexpr const char* FLAGS_DIR = "/atmosphere/contents/" SYS_FTPSRV_PROGRAM_ID_STR "/flags";
constexpr const char* BOOT2_FLAG_PATH = "/atmosphere/contents/" SYS_FTPSRV_PROGRAM_ID_STR "/flags/boot2.flag";

constexpr std::array PORT_PRESETS{21L, 2121L, 5000L, 5001L};
constexpr std::array TIMEOUT_PRESETS{0L, 30L, 60L, 120L, 300L};
// same choices as sys-ftpd-ovl.
constexpr std::array PASS_LENGTH_PRESETS{4L, 6L, 8L, 10L, 12L};

// character pools from sys-ftpd-ovl, look-alike characters (I O i l o) left out.
constexpr std::string_view PASS_UPPER = "ABCDEFGHJKLMNPQRSTUVWXYZ";
constexpr std::string_view PASS_MIXED = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";
constexpr std::string_view PASS_DIGITS = "0123456789";
constexpr std::string_view PASS_SPECIAL = "!@#$%^&*";

// how often (in frames) the server state and ip address are polled.
constexpr u32 POLL_INTERVAL = 30;
// how often (in polls, ~3s) the port is probed for sphaira's ftp server. only
// done whilst the overlay is visible and both use the same port.
constexpr u32 PROBE_EVERY_POLLS = 6;

// set once the socket driver is up, the port probe is skipped otherwise.
bool g_sockets_ready = false;

struct BoolOption {
    const char* section;
    const char* key;
    bool SysFtpConfig::* field;
};

#define SYS_FTPSRV_X_OPT(field, section, key, def) constexpr BoolOption OPT_##field{section, key, &SysFtpConfig::field};
SYS_FTPSRV_BOOL_OPTIONS(SYS_FTPSRV_X_OPT)
#undef SYS_FTPSRV_X_OPT

bool FileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool IsInstalled() {
    return FileExists(EXEFS_PATH);
}

bool IsRunning() {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, PROGRAM_ID)) && pid;
}

Result StartServer() {
    const NcmProgramLocation location{
        .program_id = PROGRAM_ID,
        .storageID = NcmStorageId_None,
    };
    u64 pid = 0;
    return pmshellLaunchProgram(0, &location, &pid);
}

Result StopServer() {
    const Result rc = pmshellTerminateProgram(PROGRAM_ID);
    if (R_FAILED(rc)) {
        return rc;
    }

    // wait for pm to finish tearing the process down so that it can be
    // relaunched straight away. bounded to ~1s.
    for (int i = 0; i < 100 && IsRunning(); i++) {
        svcSleepThread(10'000'000ULL);
    }
    return 0;
}

bool IsBootEnabled() {
    return FileExists(BOOT2_FLAG_PATH);
}

bool SetBootEnabled(bool enable) {
    if (enable) {
        mkdir(CONTENTS_DIR, 0777);
        mkdir(FLAGS_DIR, 0777);
        if (auto f = std::fopen(BOOT2_FLAG_PATH, "wb")) {
            std::fclose(f);
            return true;
        }
        return false;
    }
    return std::remove(BOOT2_FLAG_PATH) == 0 || !FileExists(BOOT2_FLAG_PATH);
}

// seeds config.ini from the shipped template so that the comments are kept
// when the overlay writes its first setting.
void EnsureConfigFromTemplate() {
    if (FileExists(SYS_FTPSRV_CONFIG_PATH) || !FileExists(SYS_FTPSRV_TEMPLATE_PATH)) {
        return;
    }

    auto in = std::fopen(SYS_FTPSRV_TEMPLATE_PATH, "rb");
    if (!in) {
        return;
    }

    if (auto out = std::fopen(SYS_FTPSRV_CONFIG_PATH, "wb")) {
        char buf[0x400];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
            std::fwrite(buf, 1, n, out);
        }
        std::fclose(out);
    }
    std::fclose(in);
}

// in network byte order, 0 if there is no connection.
u32 GetIpAddressRaw() {
    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip))) {
        return 0;
    }
    return ip;
}

std::string IpToString(u32 ip) {
    if (!ip) {
        return "";
    }

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    return buf;
}

// who is listening on a port, found by connecting to it.
enum class PortOwner {
    Unknown,    // probe failed or gave an unexpected answer.
    None,       // nothing is listening.
    SysFtpsrv,  // this sysmodule.
    Other,      // another ftp server, sphaira's if it's on sphaira's port.
};

// reads one ftp reply and returns its code, or -1 on timeout / error.
// multi-line replies end with "ddd " so wait for that line.
int ReadReplyCode(int fd, u64 deadline_ns) {
    char buf[512];
    std::size_t len = 0;

    while (len < sizeof(buf) - 1) {
        const s64 remaining_ms = (static_cast<s64>(deadline_ns) - static_cast<s64>(armTicksToNs(armGetSystemTick()))) / 1'000'000;
        if (remaining_ms <= 0) {
            return -1;
        }

        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(remaining_ms)) <= 0) {
            return -1;
        }

        const auto n = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
        if (n <= 0) {
            return -1;
        }
        len += n;
        buf[len] = '\0';

        // check each complete line for the final "ddd " line.
        const char* line = buf;
        while (const char* eol = std::strstr(line, "\r\n")) {
            if (eol - line >= 4 && std::isdigit(line[0]) && std::isdigit(line[1]) && std::isdigit(line[2]) && line[3] == ' ') {
                return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
            }
            line = eol + 2;
        }
    }

    return -1;
}

// connects to the port and identifies the server without logging in.
// sys-ftpsrv includes ftpsrv's custom nx commands and sphaira's server does
// not, so an unauthenticated "TID" (read only: reports the running title id)
// gets "530 Not logged in" from sys-ftpsrv and "500 unrecognized" from sphaira.
PortOwner ProbePort(u32 ip, long port) {
    // whole probe is bounded, ftpsrv answers immediately so this is never
    // reached in practice unless something else is on the port.
    const u64 deadline = armTicksToNs(armGetSystemTick()) + 300'000'000ULL;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return PortOwner::Unknown;
    }

    const auto close_fd = [fd](PortOwner result) {
        close(fd);
        return result;
    };

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<u16>(port));
    // our own lan address when connected, otherwise loopback.
    sa.sin_addr.s_addr = ip ? ip : htonl(INADDR_LOOPBACK);

    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        if (errno == ECONNREFUSED) {
            return close_fd(PortOwner::None);
        }
        if (errno != EINPROGRESS) {
            return close_fd(PortOwner::Unknown);
        }

        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, 100) <= 0) {
            return close_fd(PortOwner::Unknown);
        }

        int err = 0;
        socklen_t err_len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len);
        if (err == ECONNREFUSED) {
            return close_fd(PortOwner::None);
        }
        if (err) {
            return close_fd(PortOwner::Unknown);
        }
    }

    // no greeting (or e.g. "421 too many sessions", which either server may
    // send) doesn't say who it is, so only the TID reply below is trusted.
    if (ReadReplyCode(fd, deadline) != 220) {
        return close_fd(PortOwner::Unknown);
    }

    constexpr char PROBE[] = "TID\r\n";
    if (send(fd, PROBE, sizeof(PROBE) - 1, 0) != sizeof(PROBE) - 1) {
        return close_fd(PortOwner::Unknown);
    }

    const int code = ReadReplyCode(fd, deadline);
    if (code < 0) {
        return close_fd(PortOwner::Unknown);
    }

    // 500 / 502: command not recognised / implemented, so not sys-ftpsrv.
    if (code == 500 || code == 502) {
        return close_fd(PortOwner::Other);
    }

    return close_fd(PortOwner::SysFtpsrv);
}

std::string ResultToString(Result rc) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Error 0x%X", rc);
    return buf;
}

template<std::size_t N>
long NextPreset(const std::array<long, N>& presets, long current) {
    for (std::size_t i = 0; i < N; i++) {
        if (presets[i] == current) {
            return presets[(i + 1) % N];
        }
    }
    // a custom value from config.ini, start the cycle from the beginning.
    return presets[0];
}

std::string TimeoutToString(long timeout) {
    if (!timeout) {
        return "Off";
    }
    return std::to_string(timeout) + "s";
}

// uniform index in [0, n) from libnx's kernel seeded csprng. rejection
// sampling avoids the modulo bias of rand() % n.
std::size_t RandomIndex(std::size_t n) {
    const u64 limit = UINT64_MAX - (UINT64_MAX % n);
    u64 value;
    do {
        value = randomGet64();
    } while (value >= limit);
    return value % n;
}

// mirrors sys-ftpd-ovl's generator: at least one letter, one digit and (if
// enabled) one special character, then shuffled.
std::string GeneratePassword(const SysFtpConfig& cfg) {
    const auto letters = cfg.pass_uppercase_only ? PASS_UPPER : PASS_MIXED;

    std::string pool{letters};
    pool += PASS_DIGITS;
    if (cfg.pass_special_chars) {
        pool += PASS_SPECIAL;
    }

    std::string out;
    out += letters[RandomIndex(letters.size())];
    out += PASS_DIGITS[RandomIndex(PASS_DIGITS.size())];
    if (cfg.pass_special_chars) {
        out += PASS_SPECIAL[RandomIndex(PASS_SPECIAL.size())];
    }

    while (out.size() < static_cast<std::size_t>(cfg.pass_length)) {
        out += pool[RandomIndex(pool.size())];
    }

    // fisher-yates so the guaranteed characters aren't always at the front.
    for (std::size_t i = out.size() - 1; i > 0; i--) {
        std::swap(out[i], out[RandomIndex(i + 1)]);
    }

    return out;
}

std::string Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return std::string{s};
}

// parses the "users" preset list. as in sys-ftpd-ovl, "anonymous" is dropped
// (it's a toggle here) and the current username is always included.
std::vector<std::string> ParseUserPresets(const SysFtpConfig& cfg) {
    std::vector<std::string> out;
    std::string_view list{cfg.user_presets};

    while (!list.empty()) {
        const auto comma = list.find(',');
        auto name = Trim(list.substr(0, comma));
        if (!name.empty() && name != "anonymous" && std::find(out.begin(), out.end(), name) == out.end()) {
            out.emplace_back(std::move(name));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }

    if (cfg.user[0] && std::find(out.begin(), out.end(), cfg.user) == out.end()) {
        out.insert(out.begin(), cfg.user);
    }

    return out;
}

std::string JoinUserPresets(const std::vector<std::string>& presets) {
    std::string out;
    for (const auto& name : presets) {
        if (!out.empty()) {
            out += ',';
        }
        out += name;
    }
    return out;
}

enum class NoteStyle {
    Info,
    Caution,
};

// yellow for a possible conflict with sphaira, the theme's warning color (red
// by default) once sphaira's ftp server is actually running on the port.
constexpr tsl::Color CAUTION_COLOR{0xF, 0xD, 0x4, 0xF};

// small non-interactive text block placed under a list item.
void AddNote(tsl::elm::List* list, std::initializer_list<const char*> lines, NoteStyle style) {
    constexpr s32 LINE_HEIGHT = 20;
    std::vector<std::string> text(lines.begin(), lines.end());

    auto note = new tsl::elm::CustomDrawer([text = std::move(text), style](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 w, s32 h) {
        const auto color = style == NoteStyle::Caution ? CAUTION_COLOR : tsl::Color(tsl::style::color::ColorDescription);
        s32 line_y = y + LINE_HEIGHT;
        for (const auto& line : text) {
            renderer->drawString(line, false, x + 20, line_y, 15, color);
            line_y += LINE_HEIGHT;
        }
    });

    list->addItem(note, LINE_HEIGHT * lines.size() + 10);
}

// ---------------------------------------------------------------------------
// shared state, read by every page and by the status panel drawn on each.
// ---------------------------------------------------------------------------

struct State {
    SysFtpConfig cfg{};
    SphairaFtpInfo sphaira{};
    std::vector<std::string> user_presets{};
    bool installed{};
    bool running{};
    // a setting changed whilst the server was running, R restarts to apply it.
    bool dirty{};
    u32 ip_raw{};
    std::string ip{};
    std::string error{};
    PortOwner port_owner{PortOwner::Unknown};
    u32 frame_counter{};
    u32 probe_countdown{};
};

State g_state;

// sphaira's FTP Install starts its server even when "ftp_enabled" is off,
// so a matching port is a conflict regardless of that setting.
bool PortConflict() {
    return g_state.sphaira.installed && g_state.sphaira.port == g_state.cfg.port;
}

// true when a non sys-ftpsrv ftp server answers on sphaira's (shared) port.
// can't see sphaira's server when sys-ftpsrv already holds the port, as
// sphaira's then just retries in the background without a socket.
bool SphairaServerRunning() {
    return PortConflict() && g_state.port_owner == PortOwner::Other;
}

void ProbeSphaira() {
    g_state.probe_countdown = PROBE_EVERY_POLLS;
    if (!g_sockets_ready || !PortConflict()) {
        g_state.port_owner = PortOwner::Unknown;
        return;
    }

    g_state.port_owner = ProbePort(g_state.ip_raw, g_state.cfg.port);
}

void LoadState() {
    // done before any setting is written, otherwise cfgini would create a
    // bare config.ini and the template (with its comments) is never used.
    EnsureConfigFromTemplate();
    sys_ftpsrv_config_load(&g_state.cfg);
    g_state.user_presets = ParseUserPresets(g_state.cfg);
    sys_ftpsrv_get_sphaira_ftp(&g_state.sphaira);
    g_state.installed = IsInstalled();
    g_state.running = IsRunning();
    g_state.ip_raw = GetIpAddressRaw();
    g_state.ip = IpToString(g_state.ip_raw);
    ProbeSphaira();
}

// called every frame by the visible page, does the actual work every
// POLL_INTERVAL frames. returns true if something shown in a list changed.
bool PollState() {
    auto& s = g_state;
    if (++s.frame_counter < POLL_INTERVAL) {
        return false;
    }
    s.frame_counter = 0;

    // sphaira's settings may have been changed since the overlay opened.
    SphairaFtpInfo sphaira;
    sys_ftpsrv_get_sphaira_ftp(&sphaira);
    const bool sphaira_changed = sphaira.installed != s.sphaira.installed ||
        sphaira.enabled != s.sphaira.enabled || sphaira.port != s.sphaira.port;
    s.sphaira = sphaira;

    const bool running = IsRunning();
    const auto ip_raw = GetIpAddressRaw();
    const bool state_changed = running != s.running || ip_raw != s.ip_raw || sphaira_changed;

    if (state_changed) {
        if (running != s.running && !running && s.error.empty()) {
            // the server stopped without us asking, most likely a bad config.
            s.error = sys_ftpsrv_config_has_login(&s.cfg) ? "Exited unexpectedly" : "No login set";
        }
        s.running = running;
        s.ip_raw = ip_raw;
        s.ip = IpToString(ip_raw);
    }

    const auto old_owner = s.port_owner;
    if (state_changed || !s.probe_countdown || !--s.probe_countdown) {
        ProbeSphaira();
    }

    return state_changed || old_owner != s.port_owner;
}

void MarkDirty() {
    g_state.dirty = g_state.running;
    g_state.error.clear();
    // the port may have changed, re-check it on the next poll.
    g_state.probe_countdown = 1;
    if (!PortConflict()) {
        g_state.port_owner = PortOwner::Unknown;
    }
}

void SetServerRunning(bool start) {
    auto& s = g_state;
    s.error.clear();

    if (start) {
        if (!s.installed) {
            s.error = "Not installed";
        } else if (!sys_ftpsrv_config_has_login(&s.cfg)) {
            // the sysmodule would exit straight away, so don't bother.
            s.error = "No login set";
        } else if (const auto rc = StartServer(); R_FAILED(rc)) {
            s.error = ResultToString(rc);
        } else {
            s.dirty = false;
        }
    } else if (const auto rc = StopServer(); R_FAILED(rc)) {
        s.error = ResultToString(rc);
    }

    s.running = IsRunning();
    // give a freshly launched sysmodule a moment to bind before probing.
    s.probe_countdown = 1;
}

// L: stop the server.
void StopAction() {
    if (g_state.running) {
        SetServerRunning(false);
    }
}

// R: start the server, or restart it to apply pending changes. does nothing
// when it's already running with nothing to apply, to avoid needlessly
// dropping connections.
void StartOrApplyAction() {
    if (!g_state.running) {
        SetServerRunning(true);
    } else if (g_state.dirty) {
        SetServerRunning(false);
        if (!g_state.running) {
            SetServerRunning(true);
        }
    }
}

// ---------------------------------------------------------------------------
// status panel, drawn at the top of every page.
// ---------------------------------------------------------------------------

constexpr const char* GLYPH_L = "";
constexpr const char* GLYPH_R = "";

constexpr s32 PANEL_X = 14;
constexpr s32 PANEL_W = 420;
constexpr s32 PANEL_TOP = 84;
constexpr s32 PANEL_PAD = 8;
constexpr s32 PANEL_ROW_H = 24;
constexpr s32 PANEL_FONT = 16;
constexpr s32 PANEL_LABEL_X = PANEL_X + 12;
constexpr s32 PANEL_VALUE_X = PANEL_X + 112;
constexpr s32 PANEL_ROWS = 5;
constexpr s32 PANEL_H = PANEL_PAD * 2 + PANEL_ROW_H * PANEL_ROWS;
constexpr u16 HEADER_HEIGHT = PANEL_TOP + PANEL_H + 10;

enum PanelRowId {
    PanelRow_Server,
    PanelRow_Status,
    PanelRow_Address,
    PanelRow_Changes,
    PanelRow_Sphaira,
};

struct PanelRow {
    const char* label;
    std::string value;
    tsl::Color color;
    // button hint, right aligned.
    std::string hint;
};

tsl::Color FaintColor() {
    return tsl::Color(tsl::style::color::ColorDescription);
}

std::array<PanelRow, PANEL_ROWS> BuildPanel() {
    const auto& s = g_state;
    const bool conflict = PortConflict();
    const bool sphaira_running = SphairaServerRunning();
    const auto conflict_color = sphaira_running ? tsl::warningTextColor : CAUTION_COLOR;

    std::array<PanelRow, PANEL_ROWS> rows{};

    auto& server = rows[PanelRow_Server];
    server.label = "FTP Server";
    server.value = s.running ? "On" : "Off";
    server.color = s.running ? tsl::onTextColor : tsl::offTextColor;
    if (s.running) {
        server.hint = std::string(GLYPH_L) + " Stop";
    } else {
        server.hint = std::string(GLYPH_R) + " Start";
    }

    auto& status = rows[PanelRow_Status];
    status.label = "Status";
    if (!s.error.empty()) {
        status.value = s.error;
        status.color = tsl::warningTextColor;
    } else if (!s.installed) {
        status.value = "Sysmodule not installed";
        status.color = tsl::warningTextColor;
    } else if (s.running && sphaira_running) {
        // sphaira got the port first, sys-ftpsrv silently retries until it's free.
        status.value = "Blocked by sphaira";
        status.color = conflict_color;
    } else if (s.running && conflict) {
        status.value = "Running, port shared";
        status.color = conflict_color;
    } else if (s.running) {
        status.value = "Running";
        status.color = tsl::infoTextColor;
    } else {
        status.value = "Stopped";
        status.color = FaintColor();
    }

    auto& address = rows[PanelRow_Address];
    address.label = "Address";
    if (s.ip.empty()) {
        address.value = "No network";
        address.color = FaintColor();
    } else {
        address.value = s.ip + ":" + std::to_string(s.cfg.port);
        address.color = s.running ? tsl::infoTextColor : FaintColor();
    }

    auto& changes = rows[PanelRow_Changes];
    changes.label = "Changes";
    if (s.running && s.dirty) {
        changes.value = "Restart needed";
        changes.color = tsl::infoTextColor;
        changes.hint = std::string(GLYPH_R) + " Apply";
    } else {
        changes.value = "Up to date";
        changes.color = FaintColor();
    }

    auto& sphaira = rows[PanelRow_Sphaira];
    sphaira.label = "Sphaira";
    if (!s.sphaira.installed) {
        sphaira.value = "Not installed";
        sphaira.color = FaintColor();
    } else {
        // sphaira's saved settings, unless its server is seen on the port.
        sphaira.value = "Port " + std::to_string(s.sphaira.port);
        if (sphaira_running) {
            sphaira.value += ", running";
        } else {
            sphaira.value += s.sphaira.enabled ? ", setting on" : ", setting off";
        }
        sphaira.color = conflict ? conflict_color : FaintColor();
    }

    return rows;
}

// the panel row under a touch point, or -1.
int PanelRowAt(s32 x, s32 y) {
    if (x < PANEL_X || x >= PANEL_X + PANEL_W) {
        return -1;
    }

    const s32 rel = y - (PANEL_TOP + PANEL_PAD);
    if (rel < 0 || rel >= PANEL_ROW_H * PANEL_ROWS) {
        return -1;
    }
    return rel / PANEL_ROW_H;
}

class PageGui;

// libultrahand's HeaderOverlayFrame lays the list out below the header and
// leaves drawing the header to us. this draws the title + status panel, makes
// the server / changes rows tappable and stops L / R from also jumping the
// list to its top / bottom, as they start / stop the server here.
class StatusFrame final : public tsl::elm::HeaderOverlayFrame {
public:
    StatusFrame(PageGui* gui, std::string subtitle)
    : tsl::elm::HeaderOverlayFrame(HEADER_HEIGHT), m_gui(gui), m_subtitle(std::move(subtitle)) {}

    void draw(tsl::gfx::Renderer* renderer) override {
        tsl::elm::HeaderOverlayFrame::draw(renderer);

        renderer->drawString("sys-ftpsrv", false, 20, 50, 30, tsl::defaultOverlayColor);
        renderer->drawString(m_subtitle, false, 20, 70, 15, tsl::bannerVersionTextColor);

        renderer->drawRoundedRect(PANEL_X, PANEL_TOP, PANEL_W, PANEL_H, 12, renderer->aWithOpacity(tsl::tableBGColor));

        const auto rows = BuildPanel();
        for (s32 i = 0; i < PANEL_ROWS; i++) {
            const auto& row = rows[i];
            const s32 y = PANEL_TOP + PANEL_PAD + PANEL_ROW_H * i + 17;

            if (i == m_touched_row) {
                renderer->drawRect(PANEL_X, y - 17, PANEL_W, PANEL_ROW_H, renderer->aWithOpacity(tsl::clickColor));
            }

            renderer->drawString(row.label, false, PANEL_LABEL_X, y, PANEL_FONT, tsl::sectionTextColor);
            renderer->drawString(row.value, false, PANEL_VALUE_X, y, PANEL_FONT, row.color);
            if (!row.hint.empty()) {
                const auto width = renderer->getTextDimensions(row.hint, false, PANEL_FONT).first;
                renderer->drawString(row.hint, false, PANEL_X + PANEL_W - 12 - width, y, PANEL_FONT, FaintColor());
            }
        }
    }

    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX, s32 initialY) override;

    tsl::elm::Element* requestFocus(tsl::elm::Element* oldFocus, tsl::FocusDirection direction) override {
        // libultrahand turns a lone L / R press into a jump to the top /
        // bottom of the list. those buttons stop / start the server here.
        const bool jump_requested = ::jumpToTop.exchange(false, std::memory_order_acq_rel) |
            ::jumpToBottom.exchange(false, std::memory_order_acq_rel);
        if (jump_requested && oldFocus) {
            return oldFocus;
        }
        return tsl::elm::HeaderOverlayFrame::requestFocus(oldFocus, direction);
    }

private:
    PageGui* m_gui;
    std::string m_subtitle;
    int m_touched_row{-1};
};

// base for every page: a status panel on top, a scrolling list of settings
// below, L / R to stop / start the server and periodic state polling.
class PageGui : public tsl::Gui {
public:
    explicit PageGui(std::string subtitle) : m_subtitle(std::move(subtitle)) {}

    tsl::elm::Element* createUI() final {
        auto frame = new StatusFrame(this, m_subtitle);
        auto list = new tsl::elm::List();
        Build(list);
        frame->setContent(list);
        Refresh();
        return frame;
    }

    void update() override {
        if (PollState()) {
            Refresh();
        }
    }

    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos, HidAnalogStickState leftJoyStick, HidAnalogStickState rightJoyStick) override {
        // only on their own, so e.g. the overlay launch combo can't trigger them.
        const auto only = [keysDown, keysHeld](u64 key) {
            return (keysDown & key) && !(keysHeld & ~key & ALL_KEYS_MASK);
        };

        if (only(KEY_L)) {
            StopAction();
            Refresh();
            return true;
        }
        if (only(KEY_R)) {
            StartOrApplyAction();
            Refresh();
            return true;
        }
        return false;
    }

    // a tapped panel row, see StatusFrame::onTouch().
    void OnPanelRowTapped(int row) {
        if (row == PanelRow_Server) {
            if (g_state.running) {
                StopAction();
            } else {
                StartOrApplyAction();
            }
        } else if (row == PanelRow_Changes) {
            if (g_state.running && g_state.dirty) {
                StartOrApplyAction();
            }
        } else {
            return;
        }
        Refresh();
    }

protected:
    virtual void Build(tsl::elm::List* list) = 0;
    // updates list item values that depend on g_state.
    virtual void Refresh() {}

    // affects_server is false for overlay-only settings (the password
    // generator), which don't need the server to be restarted.
    void AddToggle(tsl::elm::List* list, const char* label, const BoolOption& opt, bool affects_server = true) {
        auto item = new tsl::elm::ToggleListItem(label, g_state.cfg.*opt.field);
        item->setStateChangedListener([this, item, &opt, affects_server](bool state) {
            if (sys_ftpsrv_config_set_bool(opt.section, opt.key, state)) {
                g_state.cfg.*opt.field = state;
                if (affects_server) {
                    MarkDirty();
                }
                Refresh();
            } else {
                item->setState(g_state.cfg.*opt.field);
            }
        });
        list->addItem(item);
    }

    // opens a sub page, shown with a dropdown arrow as ultrahand does.
    template<typename Page>
    void AddSubPage(tsl::elm::List* list, const char* label) {
        auto item = new tsl::elm::ListItem(label, ult::DROPDOWN_SYMBOL);
        item->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<Page>();
                return true;
            }
            return false;
        });
        list->addItem(item);
    }

private:
    std::string m_subtitle;
};

bool StatusFrame::onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX, s32 initialY) {
    // a touch that starts on the panel belongs to it until released, and only
    // acts if released on the row it started on.
    if (event == tsl::elm::TouchEvent::Touch) {
        m_touched_row = PanelRowAt(currX, currY);
        if (m_touched_row != PanelRow_Server && m_touched_row != PanelRow_Changes) {
            m_touched_row = -1;
        }
    }

    if (m_touched_row < 0) {
        return tsl::elm::HeaderOverlayFrame::onTouch(event, currX, currY, prevX, prevY, initialX, initialY);
    }

    if (event == tsl::elm::TouchEvent::Release) {
        const int row = m_touched_row;
        m_touched_row = -1;
        if (PanelRowAt(currX, currY) == row) {
            m_gui->OnPanelRowTapped(row);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// pages
// ---------------------------------------------------------------------------

class MountsPage final : public PageGui {
public:
    MountsPage() : PageGui("Mounts") {}

protected:
    void Build(tsl::elm::List* list) override {
        list->addItem(new tsl::elm::CategoryHeader("Mounts"));
        AddNote(list, {
            "With any enabled, the root lists every device",
            "and the SD card is at /sdmc:",
        }, NoteStyle::Info);
        AddToggle(list, "Album", OPT_mount_album);
        AddToggle(list, "Switch folder", OPT_mount_switch);
        AddToggle(list, "Atmosphere contents", OPT_mount_ams_contents);
        AddToggle(list, "Save data", OPT_mount_saves);
        AddToggle(list, "Save data writable", OPT_saves_writable);
        AddToggle(list, "Gamecard", OPT_mount_gamecard);
        AddNote(list, {
            "Keeps the gamecard open whilst browsing it, this",
            "may conflict with sphaira's gamecard menu.",
        }, NoteStyle::Caution);
        AddToggle(list, "Content: System", OPT_mount_content_system);
        AddToggle(list, "Content: User", OPT_mount_content_user);
        AddToggle(list, "Content: SD card", OPT_mount_content_sd);
        AddToggle(list, "BIS file systems (risky)", OPT_mount_bis_fs);
        AddToggle(list, "BIS storage (read only)", OPT_mount_bis_storage);
    }
};

class PasswordGeneratorPage final : public PageGui {
public:
    PasswordGeneratorPage() : PageGui("Password generator") {}

protected:
    void Build(tsl::elm::List* list) override {
        list->addItem(new tsl::elm::CategoryHeader("Password generator"));

        auto length_item = new tsl::elm::ListItem("Password length", std::to_string(g_state.cfg.pass_length));
        length_item->setClickListener([length_item](u64 keys) {
            if (keys & HidNpadButton_A) {
                const auto length = NextPreset(PASS_LENGTH_PRESETS, g_state.cfg.pass_length);
                if (sys_ftpsrv_config_set_long(SYS_FTPSRV_SECTION_PASSWORD, SYS_FTPSRV_KEY_PASS_LENGTH, length)) {
                    g_state.cfg.pass_length = length;
                    length_item->setValue(std::to_string(length));
                }
                return true;
            }
            return false;
        });
        list->addItem(length_item);

        AddToggle(list, "Uppercase only", OPT_pass_uppercase_only, false);
        AddToggle(list, "Special characters", OPT_pass_special_chars, false);
    }
};

class AdvancedPage final : public PageGui {
public:
    AdvancedPage() : PageGui("Advanced") {}

protected:
    void Build(tsl::elm::List* list) override {
        list->addItem(new tsl::elm::CategoryHeader("Advanced"));

        auto boot_item = new tsl::elm::ToggleListItem("Start on boot", IsBootEnabled());
        boot_item->setStateChangedListener([boot_item](bool state) {
            if (!SetBootEnabled(state)) {
                boot_item->setState(IsBootEnabled());
            }
        });
        list->addItem(boot_item);

        AddToggle(list, "Flash controller LED", OPT_led);
        AddToggle(list, "Use local time", OPT_use_localtime);
        AddToggle(list, "Keep non-ASCII names", OPT_skip_ascii_convert);
        AddToggle(list, "Write log file", OPT_log_enabled);
    }
};

class MainPage final : public PageGui {
public:
    MainPage() : PageGui(APP_VERSION) {}

protected:
    void Build(tsl::elm::List* list) override {
        if (!g_state.installed) {
            AddNote(list, {
                "Sysmodule not found at:",
                EXEFS_PATH,
            }, NoteStyle::Caution);
        }

        list->addItem(new tsl::elm::CategoryHeader("Login"));
        AddToggle(list, "Anonymous login", OPT_anon);

        // same behaviour as sys-ftpd-ovl: A cycles through the username presets.
        m_user_item = new tsl::elm::ListItem("Username");
        m_user_item->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                CycleUsername();
                return true;
            }
            return false;
        });
        list->addItem(m_user_item);

        // same behaviour as sys-ftpd-ovl: A generates a new password.
        m_pass_item = new tsl::elm::ListItem("Password");
        m_pass_item->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                RegeneratePassword();
                return true;
            }
            return false;
        });
        list->addItem(m_pass_item);

        AddToggle(list, "Read only", OPT_read_only);
        AddNote(list, {
            "Add usernames to \"users\" under [Presets] in",
            SYS_FTPSRV_CONFIG_PATH,
        }, NoteStyle::Info);

        list->addItem(new tsl::elm::CategoryHeader("Network"));

        m_port_item = new tsl::elm::ListItem("Port");
        m_port_item->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                const auto port = NextPreset(PORT_PRESETS, g_state.cfg.port);
                if (sys_ftpsrv_config_set_long(SYS_FTPSRV_SECTION_NETWORK, SYS_FTPSRV_KEY_PORT, port)) {
                    g_state.cfg.port = port;
                    MarkDirty();
                    Refresh();
                }
                return true;
            }
            return false;
        });
        list->addItem(m_port_item);

        auto timeout_item = new tsl::elm::ListItem("Idle timeout", TimeoutToString(g_state.cfg.timeout));
        timeout_item->setClickListener([this, timeout_item](u64 keys) {
            if (keys & HidNpadButton_A) {
                const auto timeout = NextPreset(TIMEOUT_PRESETS, g_state.cfg.timeout);
                if (sys_ftpsrv_config_set_long(SYS_FTPSRV_SECTION_NETWORK, SYS_FTPSRV_KEY_TIMEOUT, timeout)) {
                    g_state.cfg.timeout = timeout;
                    timeout_item->setValue(TimeoutToString(timeout));
                    MarkDirty();
                    Refresh();
                }
                return true;
            }
            return false;
        });
        list->addItem(timeout_item);

        list->addItem(new tsl::elm::CategoryHeader("More"));
        AddSubPage<MountsPage>(list, "Mounts");
        AddSubPage<PasswordGeneratorPage>(list, "Password generator");
        AddSubPage<AdvancedPage>(list, "Advanced");
    }

    void Refresh() override {
        const auto& cfg = g_state.cfg;

        if (cfg.user[0]) {
            m_user_item->setValue(cfg.user, cfg.anon);
        } else {
            m_user_item->setValue("Not set", true);
        }

        // shown in plain text, as sys-ftpd-ovl does, so it can be typed into a client.
        if (cfg.pass[0]) {
            m_pass_item->setValue(cfg.pass, cfg.anon);
        } else {
            m_pass_item->setValue("Generate", true);
        }

        // yellow: same port as sphaira. red: sphaira's server is running on it.
        m_port_item->clearValueColor();
        if (PortConflict()) {
            m_port_item->setValue(std::to_string(cfg.port) + " (same as sphaira)", false);
            m_port_item->setValueColor(SphairaServerRunning() ? tsl::warningTextColor : CAUTION_COLOR);
        } else {
            m_port_item->setValue(std::to_string(cfg.port));
        }
    }

private:
    void CycleUsername() {
        auto& presets = g_state.user_presets;
        if (presets.empty()) {
            return;
        }

        std::size_t next = 0;
        const auto it = std::find(presets.begin(), presets.end(), g_state.cfg.user);
        if (it != presets.end()) {
            next = (std::distance(presets.begin(), it) + 1) % presets.size();
        }

        const auto& user = presets[next];
        if (!sys_ftpsrv_config_set_string(SYS_FTPSRV_SECTION_LOGIN, SYS_FTPSRV_KEY_USER, user.c_str())) {
            return;
        }

        // keeps a hand-entered username in the list, so it can be cycled back to.
        sys_ftpsrv_config_set_string(SYS_FTPSRV_SECTION_PRESETS, SYS_FTPSRV_KEY_USERS, JoinUserPresets(presets).c_str());

        std::snprintf(g_state.cfg.user, sizeof(g_state.cfg.user), "%s", user.c_str());
        MarkDirty();
        Refresh();
    }

    void RegeneratePassword() {
        const auto pass = GeneratePassword(g_state.cfg);
        if (!sys_ftpsrv_config_set_string(SYS_FTPSRV_SECTION_LOGIN, SYS_FTPSRV_KEY_PASS, pass.c_str())) {
            return;
        }

        std::snprintf(g_state.cfg.pass, sizeof(g_state.cfg.pass), "%s", pass.c_str());
        MarkDirty();
        Refresh();
    }

private:
    tsl::elm::ListItem* m_user_item{};
    tsl::elm::ListItem* m_pass_item{};
    tsl::elm::ListItem* m_port_item{};
};

class SysFtpOverlay final : public tsl::Overlay {
public:
    void initServices() override {
        pmshellInitialize();
        nifmInitialize(NifmServiceType_User);

        // only used for a single short lived probe connection, so keep the
        // buffers (and the overlay's memory use) as small as possible.
        const SocketInitConfig socket_config{
            .tcp_tx_buf_size = 0x1000,
            .tcp_rx_buf_size = 0x1000,
            .tcp_tx_buf_max_size = 0,
            .tcp_rx_buf_max_size = 0,
            .udp_tx_buf_size = 0,
            .udp_rx_buf_size = 0,
            .sb_efficiency = 1,
            .num_bsd_sessions = 1,
            .bsd_service_type = BsdServiceType_User,
        };
        g_sockets_ready = R_SUCCEEDED(socketInitialize(&socket_config));
    }

    void exitServices() override {
        if (g_sockets_ready) {
            socketExit();
            g_sockets_ready = false;
        }
        nifmExit();
        pmshellExit();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        LoadState();
        return initially<MainPage>();
    }
};

} // namespace

int main(int argc, char** argv) {
    return tsl::loop<SysFtpOverlay>(argc, argv);
}
