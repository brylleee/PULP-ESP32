#pragma once

#include <iostream>
#include <string>
#include <cstdlib>
#include <cstdint>
#include <cctype>

struct NmeaNode {
    std::string token;
    NmeaNode* next;

    NmeaNode(const std::string& val) : token(val), next(nullptr) {}
};

void freeNmeaList(NmeaNode* head) {
    while (head != nullptr) {
        NmeaNode* temp = head;
        head = head->next;
        delete temp;
    }
}

inline uint32_t generateUniqueNmeaId() {
#if defined(ESP32)
    uint32_t id = esp_random();
#else
    uint32_t id = static_cast<uint32_t>(std::rand());
#endif
    return (id == 0) ? 1 : id;
}

template <typename... Args>
inline std::string buildNmea0183(uint32_t* outId, const std::string& cmd, const Args&... args) {
    uint32_t id = generateUniqueNmeaId();
    if (outId != nullptr) {
        *outId = id;
    }

    char idBuf[16];
    std::snprintf(idBuf, sizeof(idBuf), "%u", id);

    // Prepend Unique ID as the first argument after the command
    std::string payload = cmd + "," + idBuf;

    for (const std::string& arg : std::initializer_list<std::string>{args...}) {
        payload += ",";
        payload += arg;
    }

    uint8_t crc = 0;
    for (size_t i = 0; i < payload.length(); ++i) {
        crc ^= static_cast<uint8_t>(payload[i]);
    }

    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "*%02X\r\n", crc);

    return "$" + payload + suffix;
}

// Send payload to Stream (Serial)
template <typename... Args>
inline uint32_t sendNmea0183(Stream& port, const std::string& cmd, const Args&... args) {
    uint32_t id = 0;
    std::string frame = buildNmea0183(&id, cmd, args...);
    port.write(reinterpret_cast<const uint8_t*>(frame.c_str()), frame.length());
    return id;
}

NmeaNode* parseNmea0183(const std::string& rawFrame) {
    size_t startPos = rawFrame.find('$');
    size_t asteriskPos = rawFrame.find('*', startPos != std::string::npos ? startPos : 0);

    if (startPos == std::string::npos || asteriskPos == std::string::npos || asteriskPos <= startPos + 1) {
        return nullptr; // Missing delimiters or empty payload
    }

    if (asteriskPos + 2 >= rawFrame.length()) {
        return nullptr;
    }

    std::string hexCrcStr = rawFrame.substr(asteriskPos + 1, 2);
    if (!std::isxdigit(static_cast<unsigned char>(hexCrcStr[0])) ||
        !std::isxdigit(static_cast<unsigned char>(hexCrcStr[1]))) {
        return nullptr; // Invalid hex characters in checksum
    }

    uint8_t calculatedCrc = 0;
    for (size_t i = startPos + 1; i < asteriskPos; ++i) {
        calculatedCrc ^= static_cast<uint8_t>(rawFrame[i]);
    }

    uint8_t receivedCrc = static_cast<uint8_t>(std::strtoul(hexCrcStr.c_str(), nullptr, 16));
    if (calculatedCrc != receivedCrc) {
        return nullptr; // Checksum mismatch (corrupted frame)
    }

    std::string payload = rawFrame.substr(startPos + 1, asteriskPos - (startPos + 1));
    
    NmeaNode* head = nullptr;
    NmeaNode* tail = nullptr;

    size_t tokenStart = 0;
    while (true) {
        size_t commaPos = payload.find(',', tokenStart);
        std::string token = (commaPos == std::string::npos)
                            ? payload.substr(tokenStart)
                            : payload.substr(tokenStart, commaPos - tokenStart);

        NmeaNode* newNode = new NmeaNode(token);
        if (head == nullptr) {
            head = newNode;
            tail = newNode;
        } else {
            tail->next = newNode;
            tail = newNode;
        }

        if (commaPos == std::string::npos) {
            break;
        }
        tokenStart = commaPos + 1;
    }

    return head;
}