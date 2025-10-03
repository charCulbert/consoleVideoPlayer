#include "JackTransportClient.h"

#define DEBUG_PRINT(msg) do { \
    std::cout << "[JackTransport] " << msg << std::endl; \
    std::cout.flush(); \
} while(0)

JackTransportClient::JackTransportClient(const std::string& clientName) {
    jack_status_t status;
    client = jack_client_open(clientName.c_str(), JackNullOption, &status);

    if (!client) {
        errorMessage = "Failed to open JACK client";
        DEBUG_PRINT(errorMessage);
        return;
    }

    if (status & JackServerStarted) {
        DEBUG_PRINT("JACK server started");
    }

    if (status & JackNameNotUnique) {
        std::string actualName = jack_get_client_name(client);
        DEBUG_PRINT("JACK client name '" << clientName << "' was taken, using '" << actualName << "'");
    }

    // Activate the client (we're just reading transport, no audio processing)
    if (jack_activate(client)) {
        errorMessage = "Cannot activate JACK client";
        DEBUG_PRINT(errorMessage);
        jack_client_close(client);
        client = nullptr;
        return;
    }

    DEBUG_PRINT("JACK transport client initialized successfully (sample rate: "
                << getSampleRate() << " Hz)");
}

JackTransportClient::~JackTransportClient() {
    if (client) {
        jack_client_close(client);
        client = nullptr;
        DEBUG_PRINT("JACK client closed");
    }
}

jack_nframes_t JackTransportClient::getCurrentFrame() {
    if (!client) return 0;

    jack_position_t pos;
    jack_transport_query(client, &pos);
    return pos.frame;
}

bool JackTransportClient::isTransportRolling() {
    if (!client) return false;

    jack_position_t pos;
    jack_transport_state_t state = jack_transport_query(client, &pos);
    return (state == JackTransportRolling);
}

jack_nframes_t JackTransportClient::getSampleRate() {
    if (!client) return 48000;  // Default fallback
    return jack_get_sample_rate(client);
}

jack_nframes_t JackTransportClient::getPlaybackLatency() {
    if (!client) return 0;

    // Query system playback ports to get audio output latency
    const char** ports = jack_get_ports(client, "system:playback_", nullptr, JackPortIsInput);
    if (!ports || !ports[0]) {
        if (ports) jack_free(ports);
        return 0;
    }

    // Get latency from first system playback port
    jack_port_t* port = jack_port_by_name(client, ports[0]);
    jack_free(ports);

    if (!port) return 0;

    jack_latency_range_t range;
    jack_port_get_latency_range(port, JackPlaybackLatency, &range);

    // Return maximum latency (worst case for sync)
    return range.max;
}