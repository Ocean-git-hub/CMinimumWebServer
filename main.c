#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/sendfile.h>

#define PORT 8080
#define MAX_DOCUMENT_ROOT_NAME 256
#define MAX_BUFFER_SIZE 500
#define SERVER_NAME "C-Webserver"

char document_root[MAX_DOCUMENT_ROOT_NAME];

int num_preforks;

pid_t *child_pids;

pid_t parent_pid;

void perror_exit(const char *str);

inline void send_str(int socket_fd, char *str);

size_t receive_line(int socket_fd, char *buffer, size_t buffer_size);

void exchange_connection(int socket_fd);

off_t get_file_size(int fd);

void child_doing(int socket_fd);

void handle_SIGCHILD(int signal) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void handle_kill(int signal) {
    if (parent_pid != getpid())
        return;
    for (int i = 0; i < num_preforks; ++i)
        kill(child_pids[i], 9);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <document root> <back log que> <preforks>\n", argv[0]);
        return 0;
    }
    strncpy(document_root, argv[1], MAX_DOCUMENT_ROOT_NAME);
    document_root[MAX_DOCUMENT_ROOT_NAME - 1] = 0;

    int server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd == -1)
        perror_exit("Generate socket");

    int optval_true = 1;
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval_true, sizeof(optval_true)) == -1)
        perror_exit("Set socket SO_REUSEADDR");

    const struct sockaddr_in host_address = {
            .sin_family=AF_INET,
            .sin_port=htons(PORT),
            .sin_addr.s_addr=INADDR_ANY,
            .sin_zero={0}
    };

    if (bind(server_socket_fd, (const struct sockaddr *) &host_address, sizeof(struct sockaddr)) == -1)
        perror_exit("Bind socket");

    // backlog <= /proc/sys/net/core/somaxconn
    if (listen(server_socket_fd, atoi(argv[2])) == -1)
        perror_exit("Listen socket");

    signal(SIGCHLD, handle_SIGCHILD);
    signal(SIGTERM, handle_kill);
    signal(SIGINT, handle_kill);
    signal(SIGPIPE, SIG_IGN);

    printf("Startup "SERVER_NAME"\n");

    int preforks = atoi(argv[3]);
    child_pids = malloc(sizeof(pid_t) * preforks);
    parent_pid = getpid();
    for (num_preforks = 0; num_preforks < preforks;) {
        pid_t pid = fork();
        if (pid == 0)
            child_doing(server_socket_fd);
        else if (pid == -1)
            perror_exit("Fork");
        else
            child_pids[++num_preforks] = pid;
    }
    waitid(P_ALL, 0, NULL, WEXITED);

    return 1;
}

void child_doing(int socket_fd) {
    while (1) {
        int client_socket_fd;
        struct sockaddr_in client_address;
        socklen_t sockaddr_in_len = sizeof(struct sockaddr_in);
        if ((client_socket_fd = accept(socket_fd, (struct sockaddr *) &client_address, &sockaddr_in_len)) == -1)
            perror_exit("Accept connection");

        exchange_connection(client_socket_fd);
    }
}

void exchange_connection(int socket_fd) {
    char request_buffer[MAX_BUFFER_SIZE], resource_file_pass[MAX_BUFFER_SIZE], end_connection_buffer[MAX_BUFFER_SIZE],
            *buffer_pointer;
    const char default_file_name[] = "index.html";
    receive_line(socket_fd, request_buffer, MAX_BUFFER_SIZE - 10 - 1);
    if ((buffer_pointer = strstr(request_buffer, " HTTP/")) == 0) {
        shutdown(socket_fd, SHUT_RDWR);
        return;
    }

    *buffer_pointer = 0;
    if (strncmp(request_buffer, "GET ", 4) == 0)
        buffer_pointer = request_buffer + 4;
    else if (strncmp(request_buffer, "HEAD ", 5) == 0)
        buffer_pointer = request_buffer + 5;
    else {
        shutdown(socket_fd, SHUT_RDWR);
        return;
    }

    if (buffer_pointer[strlen(buffer_pointer) - 1] == '/')
        strcat(buffer_pointer, default_file_name);
    strcpy(resource_file_pass, document_root);
    strcat(resource_file_pass, buffer_pointer);

    int file_fd = open(resource_file_pass, O_RDONLY);
    off_t file_length;
    char file_length_str[20];
    if (file_fd == -1 || (file_length = get_file_size(file_fd)) == -1)
        send_str(socket_fd, "HTTP/1.0 404 Not Found\r\n"
                            "Server: "SERVER_NAME"\r\n"
                            "Connection: close\r\n"
                            "Content-Length: 80\r\n\r\n"
                            "<html><head><title>404 Not Found</title></head><body>404 Not Found</body></html>");
    else {
        sprintf(file_length_str, "%ld", file_length);
        send_str(socket_fd, "HTTP/1.0 200 OK\r\n"
                            "Server: "SERVER_NAME"\r\n"
                            "Connection: close\r\n"
                            "Content-Length: ");
        send_str(socket_fd, file_length_str);
        send_str(socket_fd, "\r\n\r\n");
        if (buffer_pointer == request_buffer + 4)
            sendfile(socket_fd, file_fd, 0, file_length);
    }
    close(file_fd);

    shutdown(socket_fd, SHUT_WR);
    while (read(socket_fd, &end_connection_buffer, sizeof(end_connection_buffer)) != 0);
    close(socket_fd);
}

void perror_exit(const char *str) {
    perror(str);
    exit(1);
}

inline void send_str(int socket_fd, char *str) {
    write(socket_fd, str, strlen(str));
}

size_t receive_line(int socket_fd, char *buffer, size_t buffer_size) {
    char *buffer_pointer = buffer, cr_lf[2] = "\r\n";
    unsigned char matched_cr_lf = 0;
    while (read(socket_fd, buffer_pointer, 1) == 1 && buffer_size != 0) {
        if (*buffer_pointer == cr_lf[matched_cr_lf]) {
            matched_cr_lf++;
            if (matched_cr_lf == 2) {
                buffer_pointer[-1] = 0;
                return strlen(buffer);
            }
        } else
            matched_cr_lf = 0;
        buffer_pointer++;
        buffer_size--;
    }
    return 0;
}

off_t get_file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) == -1)
        return -1;
    return st.st_size;
}
