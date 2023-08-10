// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2023 Bardia Moshiri <fakeshell@bardia.tech>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include "virtkey.c"

#define CONFIG_FILE "/usr/lib/droidian/device/bixby-action.conf"

typedef struct {
    char device_file[256];
    int key_code;
    char command[256];
    double wait_time;
} Config;

void background_command(const char* command) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command, NULL);
        perror("execl");
        exit(1);
    }
}

int load_config(const char* filename, Config* cfg) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        printf("Error: can't open config file\n");
        return 1;
    }

    char key[256];
    char value[256];
    while (fscanf(file, "%[^=]=%s\n", key, value) != EOF) {
        if (strcmp(key, "DEVICE_FILE") == 0) {
            strncpy(cfg->device_file, value, sizeof(cfg->device_file) - 1);
        } else if (strcmp(key, "KEY_CODE") == 0) {
            cfg->key_code = atoi(value);
        } else if (strcmp(key, "COMMAND") == 0) {
            strncpy(cfg->command, value, sizeof(cfg->command) - 1);
        } else if (strcmp(key, "WAIT_TIME") == 0) {
            cfg->wait_time = atof(value);
        } else {
            printf("Error: invalid config file format\n");
            fclose(file);
            return 1;
        }
    }
    fclose(file);

    return 0;
}

int main() {
    Config cfg;
    if (load_config(CONFIG_FILE, &cfg)) {
        return 1;
    }

    int fd = inotify_init();
    int fd_device = open(cfg.device_file, O_RDONLY);

    if (fd < 0) {
        printf("Error: inotify_init failed\n");
        return 1;
    }

    if (fd_device < 0) {
        printf("Error: Could not open device file\n");
        return 1;
    }

    int wd = inotify_add_watch(fd, cfg.device_file, IN_MODIFY);

    if (wd < 0) {
        printf("Error: inotify_add_watch failed\n");
        return 1;
    }

    struct input_event ev;

    struct wtype wtype;
    memset(&wtype, 0, sizeof(wtype));

    const char* newArgv[] = {"bixby-state", "-M", "win", "-k", "s"};
    int newArgc = sizeof(newArgv) / sizeof(newArgv[0]);

    parse_args(&wtype, newArgc, newArgv);

    wtype.display = wl_display_connect(NULL);
    if (wtype.display == NULL) {
        fail("Wayland connection failed");
    }

    wtype.registry = wl_display_get_registry(wtype.display);
    wl_registry_add_listener(wtype.registry, &registry_listener, &wtype);
    wl_display_dispatch(wtype.display);
    wl_display_roundtrip(wtype.display);

    if (wtype.manager == NULL) {
        fail("Compositor does not support the virtual keyboard protocol");
    }
    if (wtype.seat == NULL) {
        fail("No seat found");
    }

    wtype.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        wtype.manager, wtype.seat
    );

    struct timeval pressed_time;
    int is_pressed = 0;

    while (1) {
        read(fd_device, &ev, sizeof(struct input_event));
        if (ev.type == EV_KEY && ev.code == cfg.key_code) {
            if (ev.value == 1 && !is_pressed) { // Key press
                gettimeofday(&pressed_time, NULL);
                is_pressed = 1;
            } else if (ev.value == 0 && is_pressed) { // Key release
                struct timeval now;
                gettimeofday(&now, NULL);
                double elapsed = (now.tv_sec - pressed_time.tv_sec) + (now.tv_usec - pressed_time.tv_usec) / 1000000.0;

                if (elapsed > cfg.wait_time) {
                    background_command(cfg.command);
                } else {
                    upload_keymap(&wtype);
                    run_commands(&wtype);
                }

                is_pressed = 0;
            }
        }
    }

    inotify_rm_watch(fd, wd);
    close(fd);
    close(fd_device);

    free(wtype.commands);
    free(wtype.keymap);
    zwp_virtual_keyboard_v1_destroy(wtype.keyboard);
    zwp_virtual_keyboard_manager_v1_destroy(wtype.manager);
    wl_registry_destroy(wtype.registry);
    wl_display_disconnect(wtype.display);

    return 0;
}
