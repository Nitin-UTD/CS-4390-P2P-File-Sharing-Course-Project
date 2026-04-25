#include "common.h"

typedef struct {
    char ip[64];
    int port;
} PeerAddr;

typedef struct {
    char filename[MAX_FILENAME];
    long filesize;
    char description[MAX_DESC];
    char md5[MAX_MD5];
    PeerAddr peers[MAX_PEERS_PER_FILE];
    int peer_count;
} FileRecord;

static FileRecord g_files[MAX_FILES];
static int g_file_count = 0;
static char g_db_dir[256] = "tracker_db";

static void safe_copy(char *dst, size_t dst_sz, const char *src) {
    if (dst_sz == 0) {
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
}

static int find_file(const char *filename) {
    for (int i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].filename, filename) == 0) {
            return i;
        }
    }
    return -1;
}

static bool has_peer(FileRecord *fr, const char *ip, int port) {
    for (int i = 0; i < fr->peer_count; i++) {
        if (fr->peers[i].port == port && strcmp(fr->peers[i].ip, ip) == 0) {
            return true;
        }
    }
    return false;
}

static int add_peer(FileRecord *fr, const char *ip, int port) {
    if (has_peer(fr, ip, port)) {
        return 0;
    }
    if (fr->peer_count >= MAX_PEERS_PER_FILE) {
        return -1;
    }
    strncpy(fr->peers[fr->peer_count].ip, ip, sizeof(fr->peers[fr->peer_count].ip) - 1);
    fr->peers[fr->peer_count].ip[sizeof(fr->peers[fr->peer_count].ip) - 1] = '\0';
    fr->peers[fr->peer_count].port = port;
    fr->peer_count++;
    return 0;
}

static void persist_tracker_file(const FileRecord *fr) {
    if (mkdir(g_db_dir, 0777) != 0 && errno != EEXIST) {
        perror("mkdir tracker_db");
        return;
    }

    char path[512];
    if (snprintf(path, sizeof(path), "%s/%.*s.track", g_db_dir, MAX_FILENAME - 1, fr->filename) >= (int)sizeof(path)) {
        fprintf(stderr, "tracker filename too long, skipping persistence for %s\n", fr->filename);
        return;
    }
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror("fopen tracker file");
        return;
    }
    fprintf(fp, "Filename: %s\n", fr->filename);
    fprintf(fp, "Filesize: %ld\n", fr->filesize);
    fprintf(fp, "Description: %s\n", fr->description);
    fprintf(fp, "MD5: %s\n", fr->md5);
    fprintf(fp, "Peers:\n");
    for (int i = 0; i < fr->peer_count; i++) {
        fprintf(fp, "  %s %d\n", fr->peers[i].ip, fr->peers[i].port);
    }
    fclose(fp);
}

static void cmd_list(int fd) {
    char line[MAX_LINE];
    snprintf(line, sizeof(line), "BEGIN_LIST %d\n", g_file_count);
    if (send_line(fd, line) != 0) {
        return;
    }
    for (int i = 0; i < g_file_count; i++) {
        const FileRecord *f = &g_files[i];
        snprintf(line, sizeof(line), "%.*s|%ld|%.*s|%.*s|%d\n",
                 MAX_FILENAME - 1, f->filename, f->filesize, MAX_MD5 - 1, f->md5, MAX_DESC - 1, f->description, f->peer_count);
        if (send_line(fd, line) != 0) {
            return;
        }
    }
    send_line(fd, "END_LIST\n");
}

static void cmd_get(int fd, const char *filename) {
    int idx = find_file(filename);
    char line[MAX_LINE];
    if (idx < 0) {
        send_line(fd, "ERR file_not_found\n");
        return;
    }

    const FileRecord *f = &g_files[idx];
    snprintf(line, sizeof(line), "BEGIN_TRACKER %s %ld %s %s %d\n", f->filename, f->filesize, f->md5, f->description, f->peer_count);
    if (send_line(fd, line) != 0) {
        return;
    }
    for (int i = 0; i < f->peer_count; i++) {
        snprintf(line, sizeof(line), "PEER %s %d\n", f->peers[i].ip, f->peers[i].port);
        if (send_line(fd, line) != 0) {
            return;
        }
    }
    send_line(fd, "END_TRACKER\n");
}

static void cmd_create(int fd, char *args) {
    char filename[MAX_FILENAME], description[MAX_DESC], md5[MAX_MD5], ip[64];
    long filesize;
    int port;

    if (sscanf(args, "%255s %ld %511s %63s %63s %d", filename, &filesize, description, md5, ip, &port) != 6) {
        send_line(fd, "ERR usage CREATETRACKER <file> <size> <desc_no_spaces> <md5> <ip> <port>\n");
        return;
    }

    int idx = find_file(filename);
    if (idx >= 0) {
        FileRecord *f = &g_files[idx];
        if (add_peer(f, ip, port) != 0) {
            send_line(fd, "ERR peer_list_full\n");
            return;
        }
        persist_tracker_file(f);
        send_line(fd, "OK tracker_exists_peer_added\n");
        return;
    }

    if (g_file_count >= MAX_FILES) {
        send_line(fd, "ERR tracker_full\n");
        return;
    }

    FileRecord *fr = &g_files[g_file_count++];
    memset(fr, 0, sizeof(*fr));
    safe_copy(fr->filename, sizeof(fr->filename), filename);
    fr->filesize = filesize;
    safe_copy(fr->description, sizeof(fr->description), description);
    safe_copy(fr->md5, sizeof(fr->md5), md5);
    if (add_peer(fr, ip, port) != 0) {
        g_file_count--;
        send_line(fd, "ERR peer_list_full\n");
        return;
    }

    persist_tracker_file(fr);
    send_line(fd, "OK tracker_created\n");
}

static void cmd_update(int fd, char *args) {
    char filename[MAX_FILENAME], ip[64];
    int port;
    if (sscanf(args, "%255s %63s %d", filename, ip, &port) != 3) {
        send_line(fd, "ERR usage UPDATETRACKER <file> <ip> <port>\n");
        return;
    }

    int idx = find_file(filename);
    if (idx < 0) {
        send_line(fd, "ERR file_not_found\n");
        return;
    }

    FileRecord *f = &g_files[idx];
    if (add_peer(f, ip, port) != 0) {
        send_line(fd, "ERR peer_list_full\n");
        return;
    }
    persist_tracker_file(f);
    send_line(fd, "OK tracker_updated\n");
}

static void handle_client(int cfd) {
    char line[MAX_LINE];
    while (1) {
        int n = recv_line(cfd, line, sizeof(line));
        if (n <= 0) {
            return;
        }
        trim_newline(line);

        if (strcmp(line, "REQ LIST") == 0 || strcmp(line, "LIST") == 0 || strcmp(line, "<REQ LIST>") == 0) {
            cmd_list(cfd);
        } else if (strncmp(line, "GET ", 4) == 0) {
            cmd_get(cfd, line + 4);
        } else if (strncmp(line, "CREATETRACKER ", 14) == 0) {
            cmd_create(cfd, line + 14);
        } else if (strncmp(line, "UPDATETRACKER ", 14) == 0) {
            cmd_update(cfd, line + 14);
        } else if (strcmp(line, "QUIT") == 0) {
            send_line(cfd, "BYE\n");
            return;
        } else {
            send_line(cfd, "ERR unknown_command\n");
        }
    }
}

int main(int argc, char *argv[]) {
    int port = 3490;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }
    if (argc >= 3) {
        safe_copy(g_db_dir, sizeof(g_db_dir), argv[2]);
    }

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

    printf("Tracker listening on port %d, db dir '%s'\n", port, g_db_dir);

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

        handle_client(cfd);
        close(cfd);
    }

    return 0;
}
