#include "common.h"

static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

static int compute_md5(const char *path, char out[MAX_MD5]) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "md5sum '%s' 2>/dev/null", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    if (!fgets(out, MAX_MD5, fp)) {
        pclose(fp);
        snprintf(cmd, sizeof(cmd), "md5 -q '%s' 2>/dev/null", path);
        fp = popen(cmd, "r");
        if (!fp) {
            return -1;
        }
        if (!fgets(out, MAX_MD5, fp)) {
            pclose(fp);
            return -1;
        }
    }
    pclose(fp);
    for (int i = 0; out[i]; i++) {
        if (out[i] == ' ' || out[i] == '\t' || out[i] == '\n') {
            out[i] = '\0';
            break;
        }
    }
    return 0;
}

static int parse_peer_config(const char *path, char ip[64], int *port) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        if (strcmp(key, "TRACKER_IP") == 0) {
            snprintf(ip, 64, "%s", val);
        } else if (strcmp(key, "TRACKER_PORT") == 0) {
            *port = atoi(val);
        }
    }
    fclose(fp);
    return 0;
}

static int tracker_send_cmd(const char *ip, int port, const char *cmd) {
    int fd = connect_to(ip, port);
    if (fd < 0) {
        perror("connect tracker");
        return -1;
    }

    if (send_line(fd, cmd) != 0) {
        perror("send tracker");
        close(fd);
        return -1;
    }

    char line[MAX_LINE];
    while (1) {
        int n = recv_line(fd, line, sizeof(line));
        if (n <= 0) {
            break;
        }
        fputs(line, stdout);
        if (strncmp(line, "END_LIST", 8) == 0 ||
            strncmp(line, "END_TRACKER", 11) == 0 ||
            strncmp(line, "OK ", 3) == 0 ||
            strncmp(line, "ERR ", 4) == 0 ||
            strncmp(line, "BYE", 3) == 0) {
            break;
        }
    }

    close(fd);
    return 0;
}

static int command_list(const char *ip, int port) {
    return tracker_send_cmd(ip, port, "REQ LIST\n");
}

static int command_create(const char *tracker_ip, int tracker_port, const char *path,
                          const char *description, const char *serve_ip, int serve_port) {
    long sz = file_size(path);
    if (sz < 0) {
        perror("stat");
        return -1;
    }

    char md5[MAX_MD5] = {0};
    if (compute_md5(path, md5) != 0) {
        fprintf(stderr, "Failed to compute md5sum for %s\n", path);
        return -1;
    }

    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;

    char desc[MAX_DESC];
    snprintf(desc, sizeof(desc), "%s", description);
    for (size_t i = 0; desc[i]; i++) {
        if (desc[i] == ' ') {
            desc[i] = '_';
        }
    }

    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "CREATETRACKER %s %ld %s %s %s %d\n", filename, sz, desc, md5, serve_ip, serve_port);
    return tracker_send_cmd(tracker_ip, tracker_port, cmd);
}

static int command_update(const char *tracker_ip, int tracker_port, const char *filename,
                          const char *serve_ip, int serve_port) {
    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "UPDATETRACKER %s %s %d\n", filename, serve_ip, serve_port);
    return tracker_send_cmd(tracker_ip, tracker_port, cmd);
}

static int command_get(const char *tracker_ip, int tracker_port, const char *filename, const char *outfile) {
    int fd = connect_to(tracker_ip, tracker_port);
    if (fd < 0) {
        perror("connect tracker");
        return -1;
    }

    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "GET %s\n", filename);
    if (send_line(fd, cmd) != 0) {
        perror("send tracker");
        close(fd);
        return -1;
    }

    FILE *out = fopen(outfile, "w");
    if (!out) {
        perror("fopen");
        close(fd);
        return -1;
    }

    char line[MAX_LINE];
    while (1) {
        int n = recv_line(fd, line, sizeof(line));
        if (n <= 0) {
            break;
        }
        fputs(line, stdout);
        fputs(line, out);
        if (strncmp(line, "END_TRACKER", 11) == 0 || strncmp(line, "ERR ", 4) == 0) {
            break;
        }
    }

    fclose(out);
    close(fd);
    return 0;
}

static int serve_one_client(int cfd, const char *shared_dir) {
    char line[MAX_LINE];
    int n = recv_line(cfd, line, sizeof(line));
    if (n <= 0) {
        return -1;
    }
    trim_newline(line);

    if (strncmp(line, "FETCH ", 6) != 0) {
        send_line(cfd, "ERR usage FETCH <filename>\n");
        return -1;
    }

    const char *filename = line + 6;
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%.*s", shared_dir, MAX_FILENAME - 1, filename) >= (int)sizeof(path)) {
        send_line(cfd, "ERR filename_too_long\n");
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        send_line(cfd, "ERR file_not_found\n");
        return -1;
    }

    long sz = file_size(path);
    char header[128];
    snprintf(header, sizeof(header), "OK %ld\n", sz);
    if (send_line(cfd, header) != 0) {
        fclose(fp);
        return -1;
    }

    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send_all(cfd, buf, r) != 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

static int command_serve(int port, const char *shared_dir) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        die("socket");
    }

    int yes = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        die("bind");
    }
    if (listen(sfd, 16) != 0) {
        die("listen");
    }

    printf("Peer file server listening on port %d, shared_dir=%s\n", port, shared_dir);
    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }
        serve_one_client(cfd, shared_dir);
        close(cfd);
    }
    return 0;
}

static int command_download(const char *track_file, const char *dest_path) {
    FILE *fp = fopen(track_file, "r");
    if (!fp) {
        perror("fopen track");
        return -1;
    }

    char filename[MAX_FILENAME] = {0};
    long expected_size = -1;
    char line[MAX_LINE];
    char peer_ip[64] = {0};
    int peer_port = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "BEGIN_TRACKER %255s %ld", filename, &expected_size) == 2) {
            continue;
        }
        if (sscanf(line, "PEER %63s %d", peer_ip, &peer_port) == 2) {
            break;
        }
    }
    fclose(fp);

    if (peer_port == 0 || filename[0] == '\0') {
        fprintf(stderr, "No usable peer found in %s\n", track_file);
        return -1;
    }

    int pfd = connect_to(peer_ip, peer_port);
    if (pfd < 0) {
        perror("connect peer");
        return -1;
    }

    char req[MAX_LINE];
    snprintf(req, sizeof(req), "FETCH %s\n", filename);
    if (send_line(pfd, req) != 0) {
        perror("send peer");
        close(pfd);
        return -1;
    }

    if (recv_line(pfd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "No response from peer\n");
        close(pfd);
        return -1;
    }

    long size = -1;
    if (sscanf(line, "OK %ld", &size) != 1) {
        fprintf(stderr, "Peer error: %s", line);
        close(pfd);
        return -1;
    }

    FILE *out = fopen(dest_path, "wb");
    if (!out) {
        perror("fopen dest");
        close(pfd);
        return -1;
    }

    long remaining = size;
    char buf[4096];
    while (remaining > 0) {
        ssize_t n = recv(pfd, buf, remaining > (long)sizeof(buf) ? sizeof(buf) : (size_t)remaining, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            fclose(out);
            close(pfd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        fwrite(buf, 1, (size_t)n, out);
        remaining -= n;
    }

    fclose(out);
    close(pfd);

    if (remaining != 0) {
        fprintf(stderr, "Incomplete download (%ld bytes missing)\n", remaining);
        return -1;
    }

    printf("Downloaded %s (%ld bytes) from %s:%d", dest_path, size, peer_ip, peer_port);
    if (expected_size >= 0 && expected_size != size) {
        printf(" [warning: tracker expected %ld bytes]", expected_size);
    }
    printf("\n");
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s <tracker_ip> <tracker_port> list\n"
            "  %s <peer.conf> list\n"
            "  %s <tracker_ip> <tracker_port> createtracker <file_path> <description> <my_ip> <my_port>\n"
            "  %s <peer.conf> createtracker <file_path> <description> <my_ip> <my_port>\n"
            "  %s <tracker_ip> <tracker_port> updatetracker <filename> <my_ip> <my_port>\n"
            "  %s <peer.conf> updatetracker <filename> <my_ip> <my_port>\n"
            "  %s <tracker_ip> <tracker_port> get <filename> <output_track_file>\n"
            "  %s <peer.conf> get <filename> <output_track_file>\n"
            "  %s serve <shared_dir> <port>\n"
            "  %s download <track_file> <dest_path>\n",
            prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "serve") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 1;
        }
        return command_serve(atoi(argv[3]), argv[2]);
    }

    if (strcmp(argv[1], "download") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 1;
        }
        return command_download(argv[2], argv[3]);
    }

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    char tracker_ip_buf[64] = {0};
    const char *tracker_ip = NULL;
    int tracker_port = 0;
    int cmd_index = 3;

    if (strstr(argv[1], ".conf") != NULL) {
        if (parse_peer_config(argv[1], tracker_ip_buf, &tracker_port) != 0 || tracker_ip_buf[0] == '\0' || tracker_port == 0) {
            fprintf(stderr, "Invalid peer config: %s\n", argv[1]);
            return 1;
        }
        tracker_ip = tracker_ip_buf;
        cmd_index = 2;
    } else {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }
        tracker_ip = argv[1];
        tracker_port = atoi(argv[2]);
    }
    const char *cmd = argv[cmd_index];

    if (strcmp(cmd, "list") == 0) {
        return command_list(tracker_ip, tracker_port);
    }

    if (strcmp(cmd, "createtracker") == 0) {
        if (argc != cmd_index + 5) {
            usage(argv[0]);
            return 1;
        }
        return command_create(tracker_ip, tracker_port, argv[cmd_index + 1], argv[cmd_index + 2], argv[cmd_index + 3], atoi(argv[cmd_index + 4]));
    }

    if (strcmp(cmd, "updatetracker") == 0) {
        if (argc != cmd_index + 4) {
            usage(argv[0]);
            return 1;
        }
        return command_update(tracker_ip, tracker_port, argv[cmd_index + 1], argv[cmd_index + 2], atoi(argv[cmd_index + 3]));
    }

    if (strcmp(cmd, "get") == 0) {
        if (argc != cmd_index + 3) {
            usage(argv[0]);
            return 1;
        }
        return command_get(tracker_ip, tracker_port, argv[cmd_index + 1], argv[cmd_index + 2]);
    }

    usage(argv[0]);
    return 1;
}
