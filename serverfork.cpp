#include <iostream>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <pthread.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* Function Declarations */

int setup_server(const std::string& ip_address, int port);

bool make_socket_nonblocking(int socket_fd);

void send_file_to_client(int client_socket, const std::string& file_name);

void send_http_headers(int client_socket, const std::string& status_code, const std::string& mime_type, const std::string& body);

std::vector<std::string> tokenize(const std::string& str, const std::string& delimiter);

void reap_child_processes(int signal_num);

void process_client(int client_socket);

bool is_file_accessible(const std::string& file_name);

/* Global Variables */
int active_client_count;

struct sockaddr_in server_config;
struct timeval socket_timeout;

/* Main Function */
int main(int argc, char *argv[]) {
    active_client_count = 0;
    struct sockaddr_in client_address;
    socklen_t client_address_length = sizeof(client_address);

    std::string delimiter = ":";
    std::vector<std::string> parsed_args = tokenize(argv[1], ":");

    std::string server_ip = "";
    int server_port;

    if (parsed_args.size() > 2) {
        server_port = std::stoi(parsed_args.back());
        for (size_t i = 0; i < 8; ++i) {
            server_ip += parsed_args[i];
        }
    } else {
        server_port = std::stoi(parsed_args[1]);
        server_ip = parsed_args[0];
    }

    int server_socket = setup_server(server_ip, server_port);

    int reuse_address = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(int)) == -1) {
        perror("setsockopt failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    signal(SIGCHLD, reap_child_processes);

    while (true) {
        int client_socket = accept(server_socket, (struct sockaddr*)&client_address, &client_address_length);
        ++active_client_count;

        if (client_socket < 0) {
            perror("Error accepting connection");
            exit(EXIT_FAILURE);
        }

        socket_timeout.tv_sec = 2;
        socket_timeout.tv_usec = 0;

        if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&socket_timeout, sizeof(socket_timeout)) < 0) {
            perror("Error setting socket timeout");
            close(client_socket);
            continue;
        }

        if (fork() == 0) {
            close(server_socket);
            process_client(client_socket);
            close(client_socket);
            exit(0);
        } else {
            close(client_socket);
        }
    }

    close(server_socket);
    return 0;
}

/* Function Definitions */

void process_client(int client_socket) {
    const int BUFFER_SIZE = 2048;
    char buffer[BUFFER_SIZE];
    int bytes_received = recv(client_socket, buffer, BUFFER_SIZE, 0);

    if (bytes_received <= 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("Error receiving data");
        }
        return;
    }

    std::string request(buffer, bytes_received);
    std::vector<std::string> lines = tokenize(request, "\n");

    if (!lines.empty()) {
        std::vector<std::string> request_line = tokenize(lines[0], " ");
        if (request_line.size() >= 2) {
            const std::string& http_method = request_line[0];
            const std::string& requested_path = request_line[1];
            if (http_method == "GET") {
                if (requested_path == "/") {
                    send_file_to_client(client_socket, "html/index.html");
                } else if (is_file_accessible(requested_path.substr(1))) {
                    send_file_to_client(client_socket, requested_path.substr(1));
                } else {
                    send_http_headers(client_socket, "404 Not Found", "text/html", "File not found");
                }
            } else if (http_method == "HEAD") {
                if (requested_path == "/") {
                    send_http_headers(client_socket, "200 OK", "text/html", "");
                } else if (is_file_accessible(requested_path.substr(1))) {
                    send_http_headers(client_socket, "200 OK", "text/html", "");
                } else {
                    send_http_headers(client_socket, "404 Not Found", "text/html", "");
                }
            }
        }
    }
}

bool make_socket_nonblocking(int socket_fd) {
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl error");
        return false;
    }
    if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl error");
        return false;
    }
    return true;
}

int setup_server(const std::string& ip_address, int port) {
    int server_socket;
    std::string port_str = std::to_string(port);
    struct addrinfo hints, *server_info, *ptr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(ip_address.c_str(), port_str.c_str(), &hints, &server_info);
    if (status != 0) {
        std::cerr << "getaddrinfo error: " << gai_strerror(status) << std::endl;
        exit(EXIT_FAILURE);
    }

    for (ptr = server_info; ptr != nullptr; ptr = ptr->ai_next) {
        server_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (server_socket == -1) continue;

        if (bind(server_socket, ptr->ai_addr, ptr->ai_addrlen) == 0) break;

        close(server_socket);
    }

    if (ptr == nullptr) {
        perror("Error binding socket");
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 10) == -1) {
        perror("Error starting to listen");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(server_info);
    return server_socket;
}

std::vector<std::string> tokenize(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0, end;

    while ((end = str.find(delimiter, start)) != std::string::npos) {
        tokens.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
    }

    tokens.push_back(str.substr(start));
    return tokens;
}

bool is_file_accessible(const std::string& file_name) {
    struct stat buffer;
    return (stat(file_name.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode));
}

void reap_child_processes(int signal_num) {
    while (waitpid(-1, nullptr, WNOHANG) > 0);
}

void send_file_to_client(int client_socket, const std::string& file_name) {
    std::ifstream file(file_name, std::ios::binary);
    if (file.is_open()) {
        std::string file_data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(file_data.size()) + "\r\n\r\n";
        send(client_socket, headers.c_str(), headers.size(), 0);
        send(client_socket, file_data.c_str(), file_data.size(), 0);
    } else {
        send_http_headers(client_socket, "404 Not Found", "text/html", "File not found");
    }
}

void send_http_headers(int client_socket, const std::string& status_code, const std::string& mime_type, const std::string& body) {
    std::string headers = "HTTP/1.1 " + status_code + "\r\nContent-Type: " + mime_type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    send(client_socket, headers.c_str(), headers.size(), 0);
    if (!body.empty()) {
        send(client_socket, body.c_str(), body.size(), 0);
    }
}
