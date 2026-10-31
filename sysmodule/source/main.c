// Copyright (c) 2026 RandomTask
// Copyright (c) 2024 TotalJustice (ftpsrv's main_sysmod.c, which this is based on)
// SPDX-License-Identifier: MIT
//
// sys-ftpsrv: ftpsrv (the ftp server used by sphaira) running as a background
// sysmodule. based on ftpsrv's src/platform/nx/main_sysmod.c, with sphaira's
// fine grained mount options and a config shared with the companion overlay.
#include "sys_ftpsrv_config.h"

#include <ftpsrv.h>
#include <ftpsrv_vfs.h>
#include "utils.h"
#include "custom_commands.h"
#include "log/log.h"

#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <switch/services/bsd.h>

static struct SysFtpConfig g_cfg;
static struct FtpSrvConfig g_ftpsrv_config;

static void ftp_log_callback(enum FTP_API_LOG_TYPE type, const char* msg) {
    (void)type;
    log_file_write(msg);
    if (g_cfg.led) {
        led_flash();
    }
}

static void ftp_progress_callback(void) {
    if (g_cfg.led) {
        led_flash();
    }
}

static int get_mount_flags(const struct SysFtpConfig* cfg) {
    int flags = 0;
    if (cfg->mount_album) {
        flags |= VfsNxMountFlag_ALBUM;
    }
    if (cfg->mount_switch) {
        flags |= VfsNxMountFlag_SWITCH;
    }
    if (cfg->mount_ams_contents) {
        flags |= VfsNxMountFlag_AMS_CONTENTS;
    }
    if (cfg->mount_saves) {
        flags |= VfsNxMountFlag_SAVES;
        if (cfg->saves_writable) {
            flags |= VfsNxMountFlag_SAVES_WRITEABLE;
        }
    }
    if (cfg->mount_gamecard) {
        flags |= VfsNxMountFlag_GC;
    }
    if (cfg->mount_content_system) {
        flags |= VfsNxMountFlag_CONTENT_SYSTEM;
    }
    if (cfg->mount_content_user) {
        flags |= VfsNxMountFlag_CONTENT_USER;
    }
    if (cfg->mount_content_sd) {
        flags |= VfsNxMountFlag_CONTENT_SDCARD;
    }
    if (cfg->mount_bis_fs) {
        flags |= VfsNxMountFlag_BIS_FS;
    }
    if (cfg->mount_bis_storage) {
        flags |= VfsNxMountFlag_BIS_STORAGE;
    }
    return flags;
}

int main(void) {
    sys_ftpsrv_config_load(&g_cfg);

    if (g_cfg.log_enabled) {
        sys_ftpsrv_config_create_dir();
        log_file_init(SYS_FTPSRV_LOG_PATH, "sys-ftpsrv " SYS_FTPSRV_VERSION " - ftpsrv " FTPSRV_VERSION_HASH);
    }

    // exit early as an unauthenticated server is a security risk with ldn_mitm.
    if (!sys_ftpsrv_config_has_login(&g_cfg)) {
        log_file_write("user / pass / anon not set in " SYS_FTPSRV_CONFIG_PATH ", exiting");
        return EXIT_FAILURE;
    }

    g_ftpsrv_config.custom_command = CUSTOM_COMMANDS;
    g_ftpsrv_config.custom_command_count = CUSTOM_COMMANDS_SIZE;
    g_ftpsrv_config.log_callback = ftp_log_callback;
    g_ftpsrv_config.progress_callback = ftp_progress_callback;
    g_ftpsrv_config.anon = g_cfg.anon;
    g_ftpsrv_config.read_only = g_cfg.read_only;
    g_ftpsrv_config.use_localtime = g_cfg.use_localtime;
    g_ftpsrv_config.port = (unsigned)g_cfg.port;
    g_ftpsrv_config.timeout = (unsigned)g_cfg.timeout;
    strncpy(g_ftpsrv_config.user, g_cfg.user, sizeof(g_ftpsrv_config.user) - 1);
    strncpy(g_ftpsrv_config.pass, g_cfg.pass, sizeof(g_ftpsrv_config.pass) - 1);

    vfs_nx_init(NULL, 0, get_mount_flags(&g_cfg), g_cfg.skip_ascii_convert);

    int timeout = -1;
    if (g_ftpsrv_config.timeout) {
        timeout = 1000 * g_ftpsrv_config.timeout;
    }

    // runs until the process is terminated (by pm, e.g. from the overlay).
    // ftpsrv_init() fails until the network is up, so keep retrying.
    while (1) {
        ftpsrv_init(&g_ftpsrv_config);
        while (1) {
            if (ftpsrv_loop(timeout) != FTP_API_LOOP_ERROR_OK) {
                svcSleepThread(1000000000ULL);
                break;
            }
        }
        ftpsrv_exit();
    }
}

// sysmodules have no applet and should use as few fs sessions as possible.
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define TCP_TX_BUF_SIZE (1024 * 4)
#define TCP_RX_BUF_SIZE (1024 * 4)
#define TCP_TX_BUF_SIZE_MAX (1024 * 64)
#define TCP_RX_BUF_SIZE_MAX (1024 * 64)
#define UDP_TX_BUF_SIZE (0)
#define UDP_RX_BUF_SIZE (0)
#define SB_EFFICIENCY (1)

#define ALIGN_MSS(v) ((((v) + 1500 - 1) / 1500) * 1500)

#define SOCKET_TMEM_SIZE \
    ((((( \
      ALIGN_MSS(TCP_TX_BUF_SIZE_MAX ? TCP_TX_BUF_SIZE_MAX : TCP_TX_BUF_SIZE) \
    + ALIGN_MSS(TCP_RX_BUF_SIZE_MAX ? TCP_RX_BUF_SIZE_MAX : TCP_RX_BUF_SIZE)) \
    + (UDP_TX_BUF_SIZE ? ALIGN_MSS(UDP_TX_BUF_SIZE) : 0)) \
    + (UDP_RX_BUF_SIZE ? ALIGN_MSS(UDP_RX_BUF_SIZE) : 0)) \
    + 0xFFF) &~ 0xFFF) \
    * SB_EFFICIENCY

#define NUMBER_OF_SOCKETS (2)

static alignas(0x1000) u8 SOCKET_TRANSFER_MEM[SOCKET_TMEM_SIZE * NUMBER_OF_SOCKETS];

static u32 socketSelectVersion(void) {
    if (hosversionBefore(3,0,0)) {
        return 1;
    } else if (hosversionBefore(4,0,0)) {
        return 2;
    } else if (hosversionBefore(5,0,0)) {
        return 3;
    } else if (hosversionBefore(6,0,0)) {
        return 4;
    } else if (hosversionBefore(8,0,0)) {
        return 5;
    } else if (hosversionBefore(9,0,0)) {
        return 6;
    } else if (hosversionBefore(13,0,0)) {
        return 7;
    } else if (hosversionBefore(16,0,0)) {
        return 8;
    } else /* latest known version */ {
        return 9;
    }
}

void __libnx_init_time(void);

// ftpsrv uses no dynamic memory, so the sysmodule gets no heap at all.
void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = NULL;
    fake_heap_end   = NULL;
}

void __appInit(void) {
    Result rc;

    if (R_FAILED(rc = smInitialize()))
        diagAbortWithResult(rc);

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    const BsdInitConfig bsd_config = {
        .version             = socketSelectVersion(),

        .tmem_buffer         = SOCKET_TRANSFER_MEM,
        .tmem_buffer_size    = sizeof(SOCKET_TRANSFER_MEM),

        .tcp_tx_buf_size     = TCP_TX_BUF_SIZE,
        .tcp_rx_buf_size     = TCP_RX_BUF_SIZE,
        .tcp_tx_buf_max_size = TCP_TX_BUF_SIZE_MAX,
        .tcp_rx_buf_max_size = TCP_RX_BUF_SIZE_MAX,

        .udp_tx_buf_size     = UDP_TX_BUF_SIZE,
        .udp_rx_buf_size     = UDP_RX_BUF_SIZE,

        .sb_efficiency       = SB_EFFICIENCY,
    };

    if (R_FAILED(rc = timeInitialize()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = fsInitialize()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = fsdev_wrapMountSdmc()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = bsdInitialize(&bsd_config, 1, BsdServiceType_Auto)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = accountInitialize(AccountServiceType_System)))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = ncmInitialize()))
        diagAbortWithResult(rc);
    if (R_FAILED(rc = setInitialize()))
        diagAbortWithResult(rc);

    hidsysInitialize();
    __libnx_init_time();
}

void __appExit(void) {
    vfs_nx_exit();
    log_file_exit();
    hidsysExit();
    setExit();
    ncmExit();
    accountExit();
    bsdExit();
    fsdev_wrapUnmountAll();
    fsExit();
    timeExit();
    smExit();
}
