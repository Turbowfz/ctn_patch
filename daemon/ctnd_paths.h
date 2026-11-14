/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CTND_PATHS_H
#define CTND_PATHS_H

#define CTND_VERSION "3.1"

/* Shared with the module scripts. Overrides are for host-test builds only. */
#ifndef CTND_RUNTIME_DIR
#define CTND_RUNTIME_DIR "/data/adb/ctn_patch"
#endif
#define CTND_SERVICE_LOCK ".service.lock"
#define CTND_DAEMON_LOCK ".daemon.lock"
#define CTND_PID_FILE "daemon.pid"
#define CTND_STATE_FILE "restore.state"

#ifndef CTND_NAMES_PATH
#define CTND_NAMES_PATH "/proc/game_opt/task_boost/critical_task_name"
#endif
#ifndef CTND_ENABLE_PATH
#define CTND_ENABLE_PATH "/proc/game_opt/task_boost/ct_enable"
#endif
#ifndef CTND_GAMEPID_PATH
#define CTND_GAMEPID_PATH "/proc/game_opt/game_pid"
#endif
#ifndef CTND_BOOT_ID_PATH
#define CTND_BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#endif
#ifndef CTND_KMSG_PATH
#define CTND_KMSG_PATH "/dev/kmsg"
#endif

#define CTND_POLL_MS 800U
#define CTND_RECHECK_MS 30000U
#define CTND_GONE_MS 5000U
#define CTND_RETRY_MS 5000U
#define CTND_CACHE_TTL_MS 600000U
#define CTND_ALREADY_RUNNING 3

#endif
