#include <errno.h>
#include <systemd/sd-bus.h>
#include <pwd.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h> 
#include <sys/wait.h>
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

int restart_service(const logger_t* logger, const char *name)
{
    print_debug(logger, "Restart service: %s\n", name);

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

    sprint_debug(logger, "Queued service job as %s.\n", path);

finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    sd_bus_unref(bus);

    return r >= 0;
}

int run_command(const logger_t* logger, const action_cmd_t *cmd, const uint32_t timeout_ms, const char* actual_command)
{
    int stdin[2];

    if (pipe(stdin)) {
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
        
        dup2(stdin[1], 1);
        
        // switch to user
        if (cmd->user != NULL)
        {
            struct passwd *a = getpwnam(cmd->user);
            uid_t uid = a->pw_uid;
            setuid(uid);
        }
       
        execl("/bin/sh", "sh", "-c", actual_command, NULL);

        sprint_error(logger, "execl failed.\n");
        return 0;
    }
    else
    {
        const size_t buf_size = 32;
        char buf[buf_size];

        // i'm not writing
        close(stdin[1]);

        int res;
        struct timespec start;
        clock_gettime(CLOCK_REALTIME, &start);
        struct timespec now;

        const uint32_t delta_ms = 1e5; // 100ms
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
                sprint_error(logger, "Command %s took too long. Killing it and continuing.\n", actual_command);

                kill(pid, SIGTERM);
                waitpid(pid, NULL, WUNTRACED);
                return 0;
            }
        }

        int bytes_read;
        sprint_debug(logger, "Command output: ");
        while ((bytes_read = read(stdin[0], buf, buf_size - 1)) > 0)
        {
            buf[bytes_read] = '\0';
            sprint_debug_raw(logger, "%s", buf);
        }
        sprint_debug_raw(logger, "\n");

        close(stdin[0]);

        return res == pid;
    }

    return 1;
}

int log_to_file(const logger_t* logger, const action_log_t* action_log, const char* actual_line)
{
    FILE *file;

    // check if the file is beeing created
    int is_new = 0;
    if (access(action_log->path, F_OK) != 0) {
        is_new = 1;
    }

    file = fopen(action_log->path, "a");

    if (file == NULL)
    {
        sprint_error(logger, "Unable to open file: %s (Reason: %s)\n", action_log->path, strerror(errno));
        return 0;
    }
    if (is_new && action_log->header != NULL) {
        fputs(action_log->header, file);
        fputs("\n", file);
    }

    fputs(actual_line, file);
    fputs("\n", file);

    int ret_code = fclose(file) == 0;

    // set permissions for the file when 
    // it's newly created
    if (is_new && action_log->username != NULL) {
        struct passwd *user_passwd = getpwnam(action_log->username);

        int r = chown(action_log->path, user_passwd->pw_uid, user_passwd->pw_gid);

        if (r < 0) {
            sprint_error(logger, "Unable to chown log file %s: %s\n", action_log->path, strerror(errno));
        }
    }

    return ret_code;
}

int influx(const logger_t* logger, action_influx_t* action, const char* actual_line) {
    ssize_t num_ready;
    ssize_t written_bytes;
    float timeout_left = action->timeout;

    sprint_debug(logger, "[Influx]: with timeout %1.2f started\n", timeout_left);
    if (action->conn_socket <= 0) {
        // address to connect to
        struct sockaddr_storage addr;

        // calculate address
        int s;
        sa_family_t family;
        s = to_sockaddr(action->host, &addr, &family);

        // if to_sockaddr failed
        if (s == 0) {
            if (!resolve_hostname(logger, action->host, &addr, &family)) {
                sprint_error(logger, "Unable to get an IP for: %s\n", action->host);

                return 0;
            }
        }

        action->conn_socket = socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);

        if (action->conn_socket < 0) {
            sprint_debug(logger, "Unable to create socket.\n");
            return 0;
        }
        action->conn_epoll_write_fd = epoll_create(1);
        action->conn_epoll_read_fd = epoll_create(1);

        // create epoll fd for writiting
        struct epoll_event event;
        event.events = EPOLLOUT;
        event.data.fd = action->conn_socket;

        epoll_ctl(action->conn_epoll_write_fd, EPOLL_CTL_ADD, action->conn_socket, &event);

        // create epoll fd for reading
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = action->conn_socket;

        epoll_ctl(action->conn_epoll_read_fd, EPOLL_CTL_ADD, action->conn_socket, &event);

        time_t t1;
        time(&t1);
        if (family == AF_INET) {
            ((struct sockaddr_in*)&addr)->sin_port = htons(action->port);
            ((struct sockaddr_in*)&addr)->sin_family = family;

            s = connect(action->conn_socket, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
        } else {
            ((struct sockaddr_in6*)&addr)->sin6_port = htons(action->port);
            ((struct sockaddr_in6*)&addr)->sin6_family = family;

            s = connect(action->conn_socket, (struct sockaddr *) &addr, sizeof(struct sockaddr_in6));
        }
        
        // If s == 0, then we are successfully connected
        if (s == 0) {
            sprint_debug(logger, "[Influx]: Connected to %s:%d\n", action->host, action->port);
        } else if (s == -1 && errno == EINPROGRESS) {
            sprint_debug(logger, "[Influx]: Not immediately connected to %s:%d\n", action->host, action->port);
   
            struct epoll_event events_write[1];
    
            // wait for maximum 10 seconds until we can write
            num_ready = epoll_wait(action->conn_epoll_write_fd, events_write, 1, 10 * 1e3);

            if (num_ready <= 0) {
                sprint_error(logger, "[Influx]: Unable to connect to %s:%d\n", action->host, action->port);
                CLOSE(action);
                
                return 0;
            } else {
                time_t t2;
                time(&t2);

                timeout_left -= t2 - t1;
                sprint_debug(logger, "[Influx]: Succesfully connected to %s:%d\n", action->host, action->port);
            }
        } else {
            sprint_error(logger, "[Influx]: Unable to connect to %s: %s\n", action->host, strerror(errno));

            CLOSE(action);
            return 0;
        }
    } // end of creating socket

    char header[256];
    int line_len = strlen(actual_line) + 1;
    char body[line_len];

    // create body
    snprintf(body, line_len, "%s\n", actual_line);

    // header
    snprintf(header, 256, "POST %s HTTP/1.1\r\n"
                          "Host: %s:%d\r\n"
                          "Content-Length: %zd\r\n"
                          "Authorization: %s\r\n\r\n",
                          action->endpoint, action->host, action->port, strlen(body), action->authorization);

    written_bytes = 0;
    do {
        written_bytes = write(action->conn_socket, header, strlen(header));

        if (written_bytes == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            time_t t1;
            time(&t1);

            struct epoll_event events_write[1];
            num_ready = epoll_wait(action->conn_epoll_write_fd, events_write, 1, timeout_left * 1e3);

            if (num_ready <= 0) {
                sprint_error(logger, "[Influx]: Timeout while waiting for %s:%d.\n", action->host, action->port);

                CLOSE(action);
                return 0;
            }
            time_t t2;
            time(&t2);

            timeout_left -= t2 - t1;
            if (timeout_left <= 0) {
                sprint_error(logger, "[Influx]: Timeout for %s:%d\n", action->host, action->port);
                return 0; 
            }
            continue;
        } else if (written_bytes == (ssize_t)strlen(header)) {
            break;
        }
        sprint_error(logger, "[Influx]: Unable to send to %s:%d %s\n", action->host, action->port, strerror(errno));
        CLOSE(action);
        return 0;
    } while (1);

    // send the body
    do {
        written_bytes = write(action->conn_socket, body, strlen(body));

        if (written_bytes == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            time_t t1;
            struct epoll_event events[1];
            time(&t1);

            // 2 seconds timeout for waiting until server is ready to receive data
            num_ready = epoll_wait(action->conn_epoll_write_fd, events, 1, timeout_left * 1e3);

            if (num_ready <= 0) {
                sprint_error(logger, "[Influx]: Timeout while waiting for %s:%d.\n", action->host, action->port);

                CLOSE(action);
                return 0;
            }
            time_t t2;
            time(&t2);

            timeout_left -= t2 - t1;
            if (timeout_left <= 0) {
                sprint_error(logger, "[Influx]: Timeout for %s:%d\n", action->host, action->port);
                return 0; 
            }
            continue;
        } else if (written_bytes == (ssize_t)strlen(body)) {
            break;
        }
        sprint_error(logger, "[Influx]: Unable to send body to %s:%d %s\n", action->host, action->port, strerror(errno));
        CLOSE(action);
        return 0;
    } while (1);

    // we only need to look at the start to see if we were successfull at
    // writing
    int read_bytes = 0;
    char answer[128];

    do {
        read_bytes = read(action->conn_socket, answer, sizeof(answer));

        if (read_bytes == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            time_t t1;
            time(&t1);

            struct epoll_event events[1];

            // 2 seconds timeout for processing
            num_ready = epoll_wait(action->conn_epoll_read_fd, events, 1, timeout_left * 1e3);

            if (num_ready <= 0) {
                sprint_error(logger, "[Influx]: Timeout for an answer while waiting for %s:%d.\n", action->host, action->port);

                CLOSE(action);
                return 0;
            }
            continue;
        } else if (read_bytes > 22) {
            break;
        }

        sprint_error(logger, "[Influx]: Unable to receive answer from %s:%d %s\n", action->host, action->port, strerror(errno));
        CLOSE(action);
        return 0;
    } while (1);

    answer[read_bytes] = '\0';

    // check if starts with start_success
    const char* start_success = "HTTP/1.1 204 No Content";
    if (strncmp(answer, start_success, 23) == 0) {
        sprint_debug(logger, "[Influx]: Success\n");
        return 1;
    }

    sprint_error(logger, "[Influx] Not successfull writing to influxdb. Received: %s\n", answer);
    
    CLOSE(action);

    return 0;
}
