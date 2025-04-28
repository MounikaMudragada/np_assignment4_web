#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstring>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <signal.h>
#include <fcntl.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>

volatile sig_atomic_t keep_running = 1;
int server_socket = -1;

void int_handler(int) {
    keep_running = 0;
}

void sigchld_handler(int) {
    while (waitpid(-1, nullptr, WNOHANG) > 0);
}

std::vector<std::string> split_string(const std::string &text, const std::string &delimiter) {
    std::vector<std::string> parts;
    size_t start = 0, pos;
    while ((pos = text.find(delimiter, start)) != std::string::npos) {
        parts.push_back(text.substr(start, pos - start));
        start = pos + delimiter.length();
    }
    parts.push_back(text.substr(start));
    return parts;
}

bool file_exists(const std::string &file_path) {
    struct stat st;
    return (stat(file_path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
}

void send_response(int client_socket, const std::string &status, const std::string &mime_type, const std::string &message) {
    std::string response = "HTTP/1.1 " + status +
                           "\r\nContent-Type: " + mime_type +
                           "\r\nContent-Length: " + std::to_string(message.size()) +
                           "\r\nConnection: close\r\n\r\n";
    send(client_socket, response.c_str(), response.size(), 0);
    if (!message.empty())
        send(client_socket, message.c_str(), message.size(), 0);
}

void send_file_content(int client_socket, const std::string &file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (file) {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string header = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(content.size()) +
                             "\r\nConnection: close\r\n\r\n";
        send(client_socket, header.c_str(), header.size(), 0);
        send(client_socket, content.c_str(), content.size(), 0);
    } else {
        send_response(client_socket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");
    }
}

void process_request(int client_socket, const std::string &directory) {
    const int buf_size = 1024;
    std::string request;
    char buf[buf_size];
    int bytes;
    while (request.find("\r\n\r\n") == std::string::npos) {
        bytes = recv(client_socket, buf, buf_size, 0);
        if (bytes > 0) {
            request.append(buf, bytes);
        } else {
            break;
        }
    }

    if (request.find("\r\n\r\n") == std::string::npos) {
        send_response(client_socket, "400 Bad Request", "text/html", "<h1>400 Bad Request</h1>");
        return;
    }

    auto lines = split_string(request, "\r\n");
    if (lines.empty() || lines[0].empty()) {
        send_response(client_socket, "400 Bad Request", "text/html", "<h1>400 Bad Request</h1>");
        return;
    }

    auto tokens = split_string(lines[0], " ");
    if (tokens.size() < 2) {
        send_response(client_socket, "400 Bad Request", "text/html", "<h1>400 Bad Request</h1>");
        return;
    }

    std::string method = tokens[0];
    std::string resource = tokens[1];

    if (resource.empty() || resource[0] != '/') {
        send_response(client_socket, "400 Bad Request", "text/html", "<h1>400 Bad Request</h1>");
        return;
    }

    if (resource.find("..") != std::string::npos || std::count(resource.begin(), resource.end(), '/') > 1) {
        send_response(client_socket, "403 Forbidden", "text/html", "<h1>403 Forbidden</h1>");
        return;
    }

    std::string file_path = (resource == "/") ? directory + "/index.html" : directory + resource;

    if (method == "GET") {
        if (file_exists(file_path))
            send_file_content(client_socket, file_path);
        else
            send_response(client_socket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");
    } else if (method == "HEAD") {
        if (file_exists(file_path))
            send_response(client_socket, "200 OK", "text/html", "");
        else
            send_response(client_socket, "404 Not Found", "text/html", "");
    } else {
        send_response(client_socket, "405 Method Not Allowed", "text/html", "<h1>405 Method Not Allowed</h1>");
    }
}

int initialize_server(const std::string &address, int port) {
    int sockfd;
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rv = getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rv != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(rv) << "\n";
        exit(EXIT_FAILURE);
    }

    for (p = res; p != nullptr; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) continue;

        int yes = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt");
            exit(EXIT_FAILURE);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            continue;
        }
        break;
    }

    if (!p) {
        std::cerr << "Failed to bind socket\n";
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(res);

    if (listen(sockfd, 10) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

void create_default_website() {
    if (!file_exists("index.html")) {
        std::ofstream ofs("index.html");
        if (ofs) {
            ofs << "<!DOCTYPE html>\n<html><head><title>Default Website</title></head><body>"
                << "<h1>Welcome to the Default Website</h1>"
                << "<p>This page was auto-generated because no index.html was found.</p>"
                << "</body></html>";
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <IP:PORT>\n";
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, int_handler);
    signal(SIGCHLD, sigchld_handler);

    auto parts = split_string(argv[1], ":");
    if (parts.size() != 2) {
        std::cerr << "Invalid IP:PORT format\n";
        exit(EXIT_FAILURE);
    }

    std::string ip_address = parts[0];
    int port = std::stoi(parts[1]);

    if (ip_address == "ip4-localhost") ip_address = "127.0.0.1";
    else if (ip_address == "ip6-localhost") ip_address = "::1";

    create_default_website();
    server_socket = initialize_server(ip_address, port);

    std::cout << "Fork-based server running on " << ip_address << ":" << port << "\n";

    struct timeval timeout = {10, 0};

    while (keep_running) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);

        if (client_socket < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (errno == EINTR) break;
            perror("accept");
            continue;
        }

        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        pid_t pid = fork();
        if (pid == 0) { // child
            close(server_socket);
            process_request(client_socket, ".");
            close(client_socket);
            exit(EXIT_SUCCESS);
        } else if (pid > 0) {
            close(client_socket);
        } else {
            perror("fork");
            close(client_socket);
        }
    }

    close(server_socket);
    std::cout << "\nShutting down fork-based server gracefully.\n";
    return 0;
}
