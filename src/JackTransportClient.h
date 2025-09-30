#pragma once

#include <jack/jack.h>
#include <string>
#include <iostream>

class JackTransportClient {
public:
    JackTransportClient(const std::string& clientName);
    ~JackTransportClient();

    bool isInitialized() const { return client != nullptr; }
    std::string getErrorMessage() const { return errorMessage; }

    // Get current transport position in frames
    jack_nframes_t getCurrentFrame();

    // Get JACK sample rate
    jack_nframes_t getSampleRate();

private:
    jack_client_t* client = nullptr;
    std::string errorMessage;
};