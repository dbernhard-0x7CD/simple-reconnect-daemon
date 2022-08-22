#include <errno.h>
#include <systemd/sd-bus.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "actions.h"
#include "printing.h"
#include "util.h"

int restart_system(const logger_t* logger)
{
#ifdef DEBUG
    return 1; // success
#endif
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = NULL;
    sd_bus *bus = NULL;
    const char *path;
    int r;

    /* Connect to the system bus */
    r = sd_bus_open_system(&bus);
    if (r < 0)
    {
        sprint_error(logger, "Failed to connect to system bus: %s\n", strerror(-r));
        goto finish;
    }

    r = sd_bus_call_method(
        bus,
        "org.freedesktop.systemd1",         /* service to contact */
        "/org/freedesktop/systemd1",        /* object path */
        "org.freedesktop.systemd1.Manager", /* interface name */
        "Reboot",                           /* method name */
        &error,                             /* object to return error in */
        &msg,                               /* return message on success */
        "");
    if (r < 0)
    {
        print_error(logger, "Failed to issue method call: %s\n", error.message);
        goto finish;
    }

    /* Parse the response message */
    r = sd_bus_message_read(msg, "o", &path);
    if (r < 0)
    {
        sprint_error(logger, "Failed to parse response message: %s\n", strerror(-r));
        goto finish;
    }

finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(msg);
    sd_bus_unref(bus);

    return r >= 0;
}

int restart_service(const logger_t* logger, const char *name, const char *ip)
{
    print_debug(logger, "[%s]: Restart service %s\n", ip, name);

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    sd_bus *bus = NULL;
    const char *path;
    int r;

    /* Connect to the system bus */
    r = sd_bus_open_system(&bus);
    if (r < 0)
    {
        sprint_error(logger, "Failed to connect to system bus: %s\n", strerror(-r));
        goto finish;
    }
    char *prefix = "/org/freedesktop/systemd1/unit/";
    int prefix_len = strlen(prefix);
    char *service_name = malloc(prefix_len + strlen(name) + 1);

    strcpy(service_name, prefix);
    strcpy(service_name + prefix_len, name);

    sprint_debug(logger, "Object path: %s\n", service_name);

    r = sd_bus_call_method(
        bus,
        "org.freedesktop.systemd1",      /* service to contact */
        service_name,                    /* object path */
        "org.freedesktop.systemd1.Unit", /* interface name */
        "Restart",                       /* method name */
        &error,                          /* object to return error in */
        &m,                              /* return message on success */
        "s",                             /* input signature */
        "fail");
    if (r < 0)
    {
        sprint_error(logger, "Failed to issue method call: %s\n", error.message);
        free(service_name);
        goto finish;
    }
    free(service_name);

    /* Parse the response message */
    r = sd_bus_message_read(m, "o", &path);
    if (r < 0)
    {
        sprint_error(logger, "Failed to parse response message: %s\n", strerror(-r));
        goto finish;
    }

    sprint_debug(logger, "[%s]: Queued service job as %s.\n", ip, path);

finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    sd_bus_unref(bus);

    return r >= 0;
}

int run_command(const logger_t* logger, const action_cmd_t *cmd, const uint32_t timeout_ms)
{
    int stdin[2];
    int stdout[2];

    if (pipe(stdin) || pipe(stdout)) {
        sprint_error(logger, "Unable to create pipe to child\n");
        return 0;
    }

    int pid = fork();

    if (pid < 0)
    {
        sprint_error(logger, "Unable to fork. %s\n", strerror(errno));
        return 0;
    }
    else if (pid == 0)
    {
        // I am the child

        // i'm not writing to stdin (thus close it)
        close(stdin[1]);
        dup2(stdin[0], 0);

        // i'm not reading from stdout (thus close it)
        close(stdout[0]);
        dup2(stdout[1], 1);

        // switch to user
        if (cmd->user != NULL)
        {
            struct passwd *a = getpwnam(cmd->user);
            uid_t uid = a->pw_uid;
            setuid(uid);
        }
       
        execl("/bin/sh", "sh", "-c", cmd->command, NULL);

        sprint_error(logger, "execl failed.\n");
        return 0;
    }
    else
    {
        const size_t buf_size = 32;
        char buf[buf_size];

        close(stdout[1]);
        close(stdin[0]);

        // await child
        while (read(stdout[0], buf, buf_size) > 0)
        {
            sprint_debug(logger, "Command output: %s\n", buf);
        }

        int res;
        struct timespec start;
        clock_gettime(CLOCK_REALTIME, &start);
        struct timespec now;

        const uint32_t delta_ms = 1e2; // 100ms
        uint32_t diff_ms = 0;

        while ((res = waitpid(pid, NULL, WNOHANG)) == 0) {
            if (res < 0) {
                sprint_error(logger, "Unable to wait for pid %d: %s\n", pid, strerror(errno));

                return 0;
            }
            usleep(delta_ms); // sleep 100ms

            clock_gettime(CLOCK_REALTIME, &now);

            diff_ms = calculate_difference_ms(start, now);

            if (diff_ms >= timeout_ms) {
                sprint_error(logger, "Command %s took too long. Killing it and continuing.\n", cmd->command);

                kill(pid, SIGTERM);
                waitpid(pid, NULL, WUNTRACED);
                return 0;
            }
        }

        return res == pid;
    }

    return 1;
}

int log_to_file(const logger_t* logger, const char *path, const char *message, const char* username)
{
    FILE *file;

    // check if the file is beeing created
    int is_new = 0;
    if (access(path, F_OK) != 0) {
        is_new = 1;
    }

    file = fopen(path, "a");

    if (file == NULL)
    {
        print_error(logger, "Unable to open file: %s (Reason: %s)\n", path, strerror(errno));
        return 0;
    }

    fputs(message, file);
    fputs("\n", file);

    int ret_code = fclose(file) == 0;

    // set permissions for the file when 
    // it's newly created
    if (is_new && username != NULL) {
        struct passwd *user_passwd = getpwnam(username);

        int r = chown(path, user_passwd->pw_uid, user_passwd->pw_gid);

        if (r < 0) {
            sprint_error(logger, "Unable to chown log file %s: %s\n", path, strerror(errno));
        }
    }

    return ret_code;
}
