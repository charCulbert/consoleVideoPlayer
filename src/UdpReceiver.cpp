#include "UdpReceiver.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define DEBUG_PRINT(msg) do { \
    std::cout << "[UdpReceiver] " << msg << std::endl; \
    std::cout.flush(); \
} while(0)

UdpReceiver::UdpReceiver(int port) : port(port) {}

UdpReceiver::~UdpReceiver() {
    stop();
}

bool UdpReceiver::start(CommandCallback callback) {
    if (running) {
        errorMessage = "Already running";
        return false;
    }

    commandCallback = callback;

    // Create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        errorMessage = "Failed to create socket";
        return false;
    }

    // Set socket to reuse address
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        errorMessage = "Failed to set SO_REUSEADDR";
        close(sockfd);
        sockfd = -1;
        return false;
    }

    // Allow broadcast
    int broadcast = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) < 0) {
        errorMessage = "Failed to set SO_BROADCAST";
        close(sockfd);
        sockfd = -1;
        return false;
    }

    // Set non-blocking for clean shutdown
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    // Bind to port
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        errorMessage = "Failed to bind to port " + std::to_string(port);
        close(sockfd);
        sockfd = -1;
        return false;
    }

    DEBUG_PRINT("UDP receiver bound to port " << port);

    running = true;
    receiveThread = std::thread(&UdpReceiver::receiveLoop, this);

    return true;
}

void UdpReceiver::stop() {
    if (!running) return;

    running = false;

    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }

    if (receiveThread.joinable()) {
        receiveThread.join();
    }

    DEBUG_PRINT("Stopped");
}

void UdpReceiver::receiveLoop() {
    DEBUG_PRINT("Receive loop started");

    char buffer[1024];
    struct sockaddr_in clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);

    while (running) {
        ssize_t received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                                    (struct sockaddr*)&clientAddr, &clientAddrLen);

        if (received > 0) {
            buffer[received] = '\0';
            std::string command(buffer);

            // Trim whitespace
            size_t start = command.find_first_not_of(" \t\n\r");
            size_t end = command.find_last_not_of(" \t\n\r");
            if (start != std::string::npos && end != std::string::npos) {
                command = command.substr(start, end - start + 1);
            }

            if (!command.empty() && commandCallback) {
                // Only log non-SYNC commands to reduce spam
                if (command.rfind("SYNC ", 0) != 0) {
                    DEBUG_PRINT("Received command: " << command);
                }
                commandCallback(command);
            }
        } else if (received < 0) {
            // Non-blocking socket, no data available
            usleep(1000); // 1ms sleep to avoid busy loop
        }
    }

    DEBUG_PRINT("Receive loop ended");
}