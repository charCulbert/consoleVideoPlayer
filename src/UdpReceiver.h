#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>

class UdpReceiver {
public:
    using CommandCallback = std::function<void(const std::string&)>;

    UdpReceiver(int port);
    ~UdpReceiver();

    bool start(CommandCallback callback);
    void stop();

    bool isRunning() const { return running; }
    std::string getErrorMessage() const { return errorMessage; }

private:
    void receiveLoop();

    int port;
    int sockfd = -1;
    std::atomic<bool> running{false};
    std::thread receiveThread;
    CommandCallback commandCallback;
    std::string errorMessage;
};