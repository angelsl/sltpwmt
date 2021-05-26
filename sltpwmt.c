/* Copyright (C) angelsl 2021
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <pulse/pulseaudio.h>

static const char *MAX_BRIGHTNESS_PATH = "/sys/class/backlight/intel_backlight/max_brightness";
static const char *BRIGHTNESS_PATH = "/sys/class/backlight/intel_backlight/brightness";

static void rtrim(char *const str) {
    char *c = str + strlen(str);
    while (c >= str
        && (*c == '\r' || *c == '\n' || *c == ' ' || *c == '\0')) {
        --c;
    }
    *(c + 1) = '\0';
}

static ssize_t read_sysfs(const char *const path, char *const buf, ssize_t buflen) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror("read_sysfs failed (open)");
        return -1;
    }
    ssize_t rdlen = read(fd, buf, buflen - 1);
    if (rdlen == -1) {
        perror("read_sysfs failed (read)");
    }
    close(fd);
    buf[rdlen] = '\0';
    return rdlen;
}

static ssize_t write_sysfs(const char *const path, const char *const buf, ssize_t nbytes) {
    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        perror("write_sysfs failed (open)");
        return -1;
    }
    ssize_t wrlen = write(fd, buf, nbytes);
    if (wrlen == -1 || wrlen < nbytes) {
        perror("write_sysfs failed (write)");
    }
    close(fd);
    return wrlen;
}

static int do_brightness(int delta) {
    char buf[512] = {0};
    const int buflen = sizeof(buf);
    ssize_t rwlen = -1;
    if ((rwlen = read_sysfs(MAX_BRIGHTNESS_PATH, buf, buflen)) == -1) {
        return 1;
    }
    int max_br;
    if (sscanf(buf, "%d", &max_br) < 1) {
        max_br  = 2147483647;
    }

    if ((rwlen = read_sysfs(BRIGHTNESS_PATH, buf, buflen)) == -1) {
        return 1;
    }

    int br = -1;
    if (sscanf(buf, "%d", &br) < 1) {
        fprintf(stderr, "invalid brightness from sysfs\n");
        return 1;
    }
    br += delta;
    br = br < 0 ? 0 : br > max_br ? max_br : br;
    rwlen = snprintf(buf, buflen, "%d", br);
    if ((rwlen = write_sysfs(BRIGHTNESS_PATH, buf, rwlen)) == -1) {
        return 1;
    }

    rtrim(buf);
    printf("Brightness: %s", buf);
    return 0;
}

static pa_mainloop_api *pulse_mapi = NULL;
static int pulse_arg = 0;
static char pulse_op = '\0';

static void pulse_quit(int e) {
    if (pulse_mapi) {
        pulse_mapi->quit(pulse_mapi, e);
    }
}

static void do_pulse_success(pa_context *c, int success, void *userdata) {
    (void)c; (void)success; (void)userdata;
    pulse_quit(0);
}

static void do_pulse_vs(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
    (void)userdata;
    if (eol < 0) {
        fprintf(stderr, "pa_context_get_sink_info_by_name failed: %d\n", eol);
        pulse_quit(1);
        return;
    }
    if (eol) {
        return;
    }

    assert(i);

    switch (pulse_op) {
    case 's': {
        int new_mute = !i->mute;
        pa_context_set_sink_mute_by_index(c, i->index, new_mute, do_pulse_success, NULL);
        printf("%s", new_mute ? "Speakers muted" : "Speakers on");
        break;
    }

    case 'v':
        if (i->volume.channels < 1) {
            pulse_quit(0);
            return;
        }

        pa_cvolume new_cvol = i->volume;
        int new_volume = (int)pa_cvolume_max(&new_cvol) + pulse_arg;
        const int normal_volume = (int)PA_VOLUME_NORM;
        new_volume = new_volume > (normal_volume * 98 / 100) && new_volume < (normal_volume * 102 / 100)
            ? normal_volume : new_volume;
        new_volume = PA_CLAMP_UNLIKELY(new_volume, (int)PA_VOLUME_MUTED, normal_volume);
        pa_cvolume_scale(&new_cvol, new_volume);
        pa_context_set_sink_volume_by_index(c, i->index, &new_cvol, do_pulse_success, NULL);
        char buf[PA_VOLUME_SNPRINT_MAX] = {0};
        pa_volume_snprint(buf, PA_VOLUME_SNPRINT_MAX, new_volume);
        printf("Speakers %s", buf);
        break;
    default:
        fprintf(stderr, "unexpected pulse op %c in do_pulse_vs\n", pulse_op);
        pulse_quit(1);
        break;
    }
}

static void do_pulse_m(pa_context *c, const pa_source_info *i, int eol, void *userdata) {
    (void)userdata;
    if (eol < 0) {
        fprintf(stderr, "pa_context_get_source_info_by_name failed: %d\n", eol);
        pulse_quit(1);
        return;
    }
    if (eol) {
        return;
    }

    assert(i);
    int new_mute = !i->mute;
    pa_context_set_source_mute_by_index(c, i->index, new_mute, do_pulse_success, NULL);
    printf("%s", new_mute ? "Mic muted" : "Mic on");
}

static void do_pulse_server_info(pa_context *c, const pa_server_info *i, void *userdata) {
    (void)userdata;
    switch (pulse_op) {
    case 'v':
    case 's':
        pa_context_get_sink_info_by_name(c, i->default_sink_name, do_pulse_vs, NULL);
        break;
    case 'm':
        pa_context_get_source_info_by_name(c, i->default_source_name, do_pulse_m, NULL);
        break;
    }
}

static int pa_disable_sigpipe(void) {
    struct sigaction sa = {0};
    if (sigaction(SIGPIPE, NULL, &sa) < 0) {
        perror("pa_disable_sigpipe (sigaction 1)");
        return 1;
    }

    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        perror("pa_disable_sigpipe (sigaction 1)");
        return 1;
    }

    return 0;
}

static void pulse_sigint_callback(pa_mainloop_api *m, pa_signal_event *e, int sig, void *userdata) {
    (void)m; (void)e; (void)sig; (void)userdata;
    pulse_quit(0);
}

static void pulse_sm(pa_context *c, void *userdata) {
    (void)userdata;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_FAILED:
        pulse_quit(1);
        break;
    case PA_CONTEXT_READY:
        pa_context_get_server_info(c, do_pulse_server_info, NULL);
        break;
    default:
        break;
    }
}

static void print_usage(void) {
    fprintf(stderr, "usage: sltpwmt <v(olume)/b(rightness)/s(peaker toggle mute)/m(ic toggle mute)> [arg]\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    int arg = -1;
    if (argc >= 3 && sscanf(argv[2], "%d", &arg) < 1) {
        fprintf(stderr, "invalid arg value\n");
        return 1;
    }

    int ret = 1;

    switch (argv[1][0]) {
    case 'b': {
        if (argc < 3) {
            fprintf(stderr, "need arg for brightness\n");
            return 1;
        }
        ret = do_brightness(arg);
        break;
    }
    case 'v':
        if (argc < 3) {
            fprintf(stderr, "need arg for volume\n");
            return 1;
        }
        // fallthrough
    case 's':
    case 'm': {
        pulse_arg = arg;
        pulse_op = argv[1][0];

        pa_mainloop *m = NULL;
        if (!(m = pa_mainloop_new())) {
            fprintf(stderr, "pa_mainloop_new failed\n");
            return 1;
        }
        pulse_mapi = pa_mainloop_get_api(m);
        if (pa_signal_init(pulse_mapi)) {
            fprintf(stderr, "pa_signal_init failed\n");
            pulse_quit(1);
            return 1;
        }
        pa_signal_new(SIGINT, pulse_sigint_callback, NULL);
        pa_signal_new(SIGTERM, pulse_sigint_callback, NULL);
        pa_disable_sigpipe();

        pa_context *pulse_context = NULL;
        if (!(pulse_context = pa_context_new(pulse_mapi, NULL))) {
            fprintf(stderr, "pa_context_new failed\n");
            ret = 1;
            goto exit;
        }
        pa_context_set_state_callback(pulse_context, pulse_sm, NULL);
        if (pa_context_connect(pulse_context, NULL, 0, NULL) < 0) {
            fprintf(stderr, "pa_context_connect failed: %s\n", pa_strerror(pa_context_errno(pulse_context)));
            ret = 1;
            goto exit;
        }

        if (pa_mainloop_run(m, &ret) < 0) {
            fprintf(stderr, "pa_mainloop_run failed\n");
            ret = 1;
            goto exit;
        }
    exit:
        if (pulse_context) {
            pa_context_unref(pulse_context);
        }

        if (m) {
            pa_signal_done();
            pa_mainloop_free(m);
        }
        break;
    }
    default:
        fprintf(stderr, "unknown action\n");
        ret = 1;
        break;
    }

    fflush(stdout);
    return ret;
}
