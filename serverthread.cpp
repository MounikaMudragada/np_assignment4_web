#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <cstring>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <pthread.h>
#include <algorithm> // For std::count

// Global counter for connected clients
int connection_count = 0;

// Function declarations
int initialize_server(const std::string &address, int port);
bool file_exists(const std::string &file_path);
void process_request(int client_socket, const std::string &directory);
void send_file_content(int client_socket, const std::string &file_path);
void send_response(int client_socket, const std::string &status, const std::string &mime_type, const std::string &message);
std::vector<std::string> split_string(const std::string &text, const std::string &separator);

// Thread handler
void *handle_connection(void *client_socket_ptr);

int main(int argc, char *argv[]) {
    connection_count = 0;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Verify command-line arguments
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <IP:PORT>\n";
        exit(EXIT_FAILURE);
    }

    // Extract IP and port from arguments
    std::vector<std::string> address_parts = split_string(argv[1], ":");
    std::string ip_address;
    int port;

    if (address_parts.size() == 2) {
        ip_address = address_parts[0];
        port = std::stoi(address_parts[1]);
    } else {
        std::cerr << "Invalid IP:PORT format.\n";
        exit(EXIT_FAILURE);
    }

    // Create and set up the server socket
    int server_socket = initialize_server(ip_address, port);

    // Configure socket to reuse the address and port
    int reuse_option = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_option, sizeof(reuse_option)) == -1) {
        perror("Failed to set socket options");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    while (true) {
        int *client_socket = new int;
        *client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_len);
        connection_count++;

        if (*client_socket < 0) {
            perror("Error accepting connection");
            delete client_socket;
            continue;
        }

        // Set timeout for receiving data
        struct timeval timeout = {1, 0};
        if (setsockopt(*client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            perror("Failed to set receive timeout");
            close(*client_socket);
            delete client_socket;
            continue;
        }

        // Handle each client in a separate thread
        pthread_t client_thread;
        if (pthread_create(&client_thread, nullptr, handle_connection, (void *)client_socket) != 0) {
            perror("Failed to create thread");
            close(*client_socket);
            delete client_socket;
            continue;
        }

        // Detach the thread to free resources after execution
        pthread_detach(client_thread);
    }

    close(server_socket);
    return 0;
}

void *handle_connection(void *client_socket_ptr) {
    int client_socket = *(int *)client_socket_ptr;
    delete (int *)client_socket_ptr;

    process_request(client_socket, ".");
    close(client_socket);
    pthread_exit(nullptr);
}

void process_request(int client_socket, const std::string &directory) {
    const int buffer_size = 2048;
    char buffer[buffer_size] = {0};
    int bytes_received = recv(client_socket, buffer, buffer_size, 0);

    if (bytes_received <= 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("Error receiving data");
        }
        return;
    }

    std::string request(buffer, bytes_received);
    std::vector<std::string> request_lines = split_string(request, "\n");
    if (!request_lines.empty()) {
        std::vector<std::string> first_line_parts = split_string(request_lines[0], " ");
        if (first_line_parts.size() >= 2) {
            std::string method = first_line_parts[0];
            std::string resource_path = first_line_parts[1];

            // Restrict resource paths to the current directory
            if (std::count(resource_path.begin(), resource_path.end(), '/') > 3) {
                send_response(client_socket, "403 Forbidden", "text/html", "<h1>403 Forbidden</h1>");
                return;
            }

            if (method == "GET") {
                if (resource_path == "/") {
                    send_file_content(client_socket, directory + "/index.html");
                } else if (file_exists(directory + resource_path)) {
                    send_file_content(client_socket, directory + resource_path);
                } else {
                    send_response(client_socket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");
                }
            } else if (method == "HEAD") {
                if (resource_path == "/") {
                    send_response(client_socket, "200 OK", "text/html", "");
                } else if (file_exists(directory + resource_path)) {
                    send_response(client_socket, "200 OK", "text/html", "");
                } else {
                    send_response(client_socket, "404 Not Found", "text/html", "");
                }
            }
        }
    }
}

void send_file_content(int client_socket, const std::string &file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (file.is_open()) {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string response_header = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(content.size()) + "\r\n\r\n";
        send(client_socket, response_header.c_str(), response_header.size(), 0);
        send(client_socket, content.c_str(), content.size(), 0);
    } else {
        send_response(client_socket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");
    }
}

void send_response(int client_socket, const std::string &status, const std::string &mime_type, const std::string &message) {
    std::string response = "HTTP/1.1 " + status + "\r\nContent-Type: " + mime_type + "\r\nContent-Length: " + std::to_string(message.size()) + "\r\n\r\n";
    send(client_socket, response.c_str(), response.size(), 0);
    send(client_socket, message.c_str(), message.size(), 0);
}

int initialize_server(const std::string &address, int port) {
    int socket_fd;
    struct addrinfo hints{}, *res, *ptr;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        perror("Error resolving address");
        exit(EXIT_FAILURE);
    }

    for (ptr = res; ptr != nullptr; ptr = ptr->ai_next) {
        socket_fd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (socket_fd == -1) continue;

        if (bind(socket_fd, ptr->ai_addr, ptr->ai_addrlen) == 0) break;

        close(socket_fd);
    }

    if (ptr == nullptr) {
        perror("Error binding socket");
        exit(EXIT_FAILURE);
    }

    if (listen(socket_fd, 10) == -1) {
        perror("Error starting listener");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(res);
    return socket_fd;
}

bool file_exists(const std::string &file_path) {
    std::ifstream file(file_path);
    return file.good();
}

std::vector<std::string> split_string(const std::string &text, const std::string &separator) {
    std::vector<std::string> parts;
    size_t start = 0, end;
    while ((end = text.find(separator, start)) != std::string::npos) {
        parts.push_back(text.substr(start, end - start));
        start = end + separator.length();
    }
    parts.push_back(text.substr(start));
    return parts;
}
