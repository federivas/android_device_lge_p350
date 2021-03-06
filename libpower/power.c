/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (c) 2012 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_TAG "CM PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define SCALING_GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#define BOOSTPULSE_ONDEMAND "/sys/devices/system/cpu/cpufreq/ondemand/boostpulse"
#define BOOSTPULSE_INTERACTIVE "/sys/devices/system/cpu/cpufreq/interactive/boostpulse"
#define SAMPLING_RATE_ONDEMAND "/sys/devices/system/cpu/cpufreq/ondemand/sampling_rate"
#define SAMPLING_RATE_SCREEN_ON "50000"
#define SAMPLING_RATE_SCREEN_OFF "500000"

struct cm_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    int boostpulse_fd;
    int boostpulse_warned;
};

static int sysfs_read(char *path, char *s, int num_bytes)
{
    char buf[80];
    int count;
    int ret = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);

        return -1;
    }

    if ((count = read(fd, s, num_bytes - 1)) < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);

        ret = -1;
    } else {
        s[count] = '\0';
    }

    close(fd);

    return ret;
}

static void sysfs_write(char *path, char *s)
{
    char buf[80];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
    }

    close(fd);
}

static int get_scaling_governor(char governor[], int size) {
    if (sysfs_read(SCALING_GOVERNOR_PATH, governor,
                size) == -1) {
        // Can't obtain the scaling governor. Return.
        return -1;
    } else {
        // Strip newline at the end.
        int len = strlen(governor);

        len--;

        while (len >= 0 && (governor[len] == '\n' || governor[len] == '\r'))
            governor[len--] = '\0';
    }

    return 0;
}

static int boostpulse_open(struct cm_power_module *cm)
{
    char buf[80];
    char governor[80];

    pthread_mutex_lock(&cm->lock);

    if (cm->boostpulse_fd < 0) {
        if (get_scaling_governor(governor, sizeof(governor)) < 0) {
            ALOGE("Can't read scaling governor.");
            cm->boostpulse_warned = 1;
        } else {
            if (strncmp(governor, "ondemand", 8) == 0)
                cm->boostpulse_fd = open(BOOSTPULSE_ONDEMAND, O_WRONLY);
            else if (strncmp(governor, "interactive", 11) == 0)
                cm->boostpulse_fd = open(BOOSTPULSE_INTERACTIVE, O_WRONLY);

            if (cm->boostpulse_fd < 0 && !cm->boostpulse_warned) {
                strerror_r(errno, buf, sizeof(buf));
                ALOGE("Error opening boostpulse: %s\n", buf);
                cm->boostpulse_warned = 1;
            } else if (cm->boostpulse_fd > 0)
                ALOGD("Opened %s boostpulse interface", governor);
        }
    }

    pthread_mutex_unlock(&cm->lock);
    return cm->boostpulse_fd;
}

static void cm_power_hint(struct power_module *module, power_hint_t hint,
                            void *data)
{
    struct cm_power_module *cm = (struct cm_power_module *) module;
    char buf[80];
    int len;
    int duration = 1;

    switch (hint) {
    case POWER_HINT_INTERACTION:
    case POWER_HINT_CPU_BOOST:
        if (boostpulse_open(cm) >= 0) {
            if (data != NULL)
                duration = (int) data;

            snprintf(buf, sizeof(buf), "%d", duration);
            len = write(cm->boostpulse_fd, buf, strlen(buf));

            if (len < 0) {
                strerror_r(errno, buf, sizeof(buf));
	            ALOGE("Error writing to boostpulse: %s\n", buf);

                pthread_mutex_lock(&cm->lock);
                close(cm->boostpulse_fd);
                cm->boostpulse_fd = -1;
                cm->boostpulse_warned = 0;
                pthread_mutex_unlock(&cm->lock);
            }
        }
        break;

    case POWER_HINT_VSYNC:
        break;

    default:
        break;
    }
}

static void cm_power_set_interactive(struct power_module *module, int on)
{
    char governor[80];

    if (strncmp(governor, "ondemand", 8) == 0)
        sysfs_write(SAMPLING_RATE_ONDEMAND,
                on ? SAMPLING_RATE_SCREEN_ON : SAMPLING_RATE_SCREEN_OFF);
    else
        ALOGV("Skipping sysfs_write to sampling_rate -- NOT using ondemand");
}

static void cm_power_init(struct power_module *module)
{
    char governor[80];

    if (strncmp(governor, "ondemand", 8) == 0)
        sysfs_write(SAMPLING_RATE_ONDEMAND, SAMPLING_RATE_SCREEN_ON);
    else
        ALOGV("Skipping sysfs_write to sampling_rate -- NOT using ondemand");
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct cm_power_module HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            version_major: 1,
            version_minor: 0,
            id: POWER_HARDWARE_MODULE_ID,
            name: "CM Power HAL",
            author: "The CyanogenMod Project",
            methods: &power_module_methods,
        },
       init: cm_power_init,
       setInteractive: cm_power_set_interactive,
       powerHint: cm_power_hint,
    },

    lock: PTHREAD_MUTEX_INITIALIZER,
    boostpulse_fd: -1,
    boostpulse_warned: 0,
};
