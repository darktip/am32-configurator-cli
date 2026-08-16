#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t EepromAddressG071 = 0x7e00;
constexpr std::uint16_t EepromAddressF051 = 0x7c00;
constexpr std::uint16_t EepromAddressF3 = 0xf800;
constexpr std::uint16_t EepromAddressNxp = 0xe000;
constexpr std::size_t ConfigSize = 48;
constexpr std::size_t DirectionByteIndex = 17;
constexpr std::uint8_t FourWayPcMarker = 0x2f;
constexpr std::uint8_t FourWayEscMarker = 0x2e;
constexpr int DefaultConnectAttempts = 30;
constexpr int DefaultConnectDelayMs = 300;
constexpr int DefaultPassthroughDelayMs = 2000;

enum class ExitCode : int {
    Success = 0,
    InvalidArguments = 1,
    ConfigFileError = 2,
    SerialOpenError = 3,
    SerialIoError = 4,
    PassthroughError = 5,
    EscConnectError = 6,
    EscWriteError = 7,
    ProtocolError = 8,
};

struct Options {
    std::string port;
    int motorIndex = -1;
    std::string configPath;
    int connectAttempts = DefaultConnectAttempts;
    int connectDelayMs = DefaultConnectDelayMs;
    int passthroughDelayMs = DefaultPassthroughDelayMs;
    bool verifyReadback = true;
    bool resetEscAfterWrite = false;
    bool resetFcAfterWrite = false;
    bool reverse = false;
    bool help = false;
};

struct ParsedFourWayResponse {
    Bytes frame;
    std::string error;
};

class SerialIoException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ProtocolException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void logInfo(const std::string& message) {
    std::cout << "[INFO] " << message << '\n';
}

void logWarn(const std::string& message) {
    std::cout << "[WARN] " << message << '\n';
}

void logError(const std::string& message) {
    std::cout << "[ERROR] " << message << '\n';
}

std::string hexByte(std::uint8_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<int>(value);
    return out.str();
}

std::string hexDump(const Bytes& data, std::size_t maxBytes = 64) {
    std::ostringstream out;
    const auto count = std::min(data.size(), maxBytes);
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<int>(data[i]);
    }
    if (data.size() > maxBytes) {
        out << " ...";
    }
    return out.str();
}

std::string hexWord(std::uint16_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
        << value;
    return out.str();
}

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  am32-cli --port COM4 --index 0 --config path\\to\\config.bin [--reverse]\n\n"
        << "Required arguments:\n"
        << "  --port <port>       Serial port, for example COM4.\n"
        << "  --index <0-3>       ESC/motor index behind the flight-controller passthrough.\n"
        << "  --config <path>     AM32 EEPROM config binary. First 48 bytes are written.\n\n"
        << "Optional arguments:\n"
        << "  --reverse           Set EEPROM direction byte 17 to reverse before writing.\n"
        << "  --connect-attempts <count>\n"
        << "                      ESC connect attempts before failing. Default: 30.\n"
        << "  --connect-delay-ms <ms>\n"
        << "                      Delay between failed ESC connect attempts. Default: 300.\n"
        << "  --passthrough-delay-ms <ms>\n"
        << "                      Delay after MSP passthrough before ESC connect. Default: 2000.\n"
        << "  --no-verify         Skip EEPROM readback after write.\n"
        << "  --reset-esc-after-write\n"
        << "                      Reset the target ESC after write. Disabled by default.\n"
        << "  --reset-fc-after-write\n"
        << "                      Send MSP FC reset after exiting four-way. Disabled by default.\n"
        << "  --help              Show this help.\n\n"
        << "Exit codes:\n"
        << "  0 success\n"
        << "  1 invalid arguments\n"
        << "  2 config file error\n"
        << "  3 serial open/configuration error\n"
        << "  4 serial read/write error\n"
        << "  5 passthrough setup error\n"
        << "  6 ESC connect error\n"
        << "  7 ESC EEPROM write error\n"
        << "  8 protocol/CRC error\n";
}

std::optional<std::string> takeValue(int& i, int argc, char* argv[], const std::string& arg) {
    const auto equals = arg.find('=');
    if (equals != std::string::npos) {
        return arg.substr(equals + 1);
    }
    if (i + 1 >= argc) {
        return std::nullopt;
    }
    return argv[++i];
}

std::optional<int> parseIntInRange(const std::string& value,
                                   int minimum,
                                   int maximum,
                                   const std::string& optionName,
                                   std::string& error) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed, 10);
        if (consumed != value.size() || parsed < minimum || parsed > maximum) {
            error = optionName + " must be an integer in range " + std::to_string(minimum) + "-" + std::to_string(maximum);
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        error = optionName + " must be an integer in range " + std::to_string(minimum) + "-" + std::to_string(maximum);
        return std::nullopt;
    }
}

std::optional<Options> parseArguments(int argc, char* argv[], std::string& error) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            options.help = true;
            return options;
        }

        if (arg == "--reverse") {
            options.reverse = true;
            continue;
        }

        if (arg == "--no-verify") {
            options.verifyReadback = false;
            continue;
        }

        if (arg == "--reset-esc-after-write") {
            options.resetEscAfterWrite = true;
            continue;
        }

        if (arg == "--reset-fc-after-write") {
            options.resetFcAfterWrite = true;
            continue;
        }

        if (arg == "--port" || arg.rfind("--port=", 0) == 0) {
            auto value = takeValue(i, argc, argv, arg);
            if (!value || value->empty()) {
                error = "--port requires a value";
                return std::nullopt;
            }
            options.port = *value;
            continue;
        }

        if (arg == "--index" || arg.rfind("--index=", 0) == 0) {
            auto value = takeValue(i, argc, argv, arg);
            if (!value || value->empty()) {
                error = "--index requires a value";
                return std::nullopt;
            }
            auto parsed = parseIntInRange(*value, 0, 3, "--index", error);
            if (!parsed) {
                return std::nullopt;
            }
            options.motorIndex = *parsed;
            continue;
        }

        if (arg == "--config" || arg.rfind("--config=", 0) == 0) {
            auto value = takeValue(i, argc, argv, arg);
            if (!value || value->empty()) {
                error = "--config requires a value";
                return std::nullopt;
            }
            options.configPath = *value;
            continue;
        }

        if (arg == "--connect-attempts" || arg.rfind("--connect-attempts=", 0) == 0) {
            auto value = takeValue(i, argc, argv, arg);
            if (!value || value->empty()) {
                error = "--connect-attempts requires a value";
                return std::nullopt;
            }
            auto parsed = parseIntInRange(*value, 1, 120, "--connect-attempts", error);
            if (!parsed) {
                return std::nullopt;
            }
            options.connectAttempts = *parsed;
            continue;
        }

        if (arg == "--connect-delay-ms" || arg.rfind("--connect-delay-ms=", 0) == 0) {
            auto value = takeValue(i, argc, argv, arg);
            if (!value || value->empty()) {
                error = "--connect-delay-ms requires a value";
                return std::nullopt;
            }
            auto parsed = parseIntInRange(*value, 0, 5000, "--connect-delay-ms", error);
            if (!parsed) {
                return std::nullopt;
            }
            options.connectDelayMs = *parsed;
            continue;
        }

        if (arg == "--passthrough-delay-ms" || arg.rfind("--passthrough-delay-ms=", 0) == 0) {
            auto value = takeValue(i, argc, argv, arg);
            if (!value || value->empty()) {
                error = "--passthrough-delay-ms requires a value";
                return std::nullopt;
            }
            auto parsed = parseIntInRange(*value, 0, 10000, "--passthrough-delay-ms", error);
            if (!parsed) {
                return std::nullopt;
            }
            options.passthroughDelayMs = *parsed;
            continue;
        }

        error = "unknown argument: " + arg;
        return std::nullopt;
    }

    if (options.port.empty()) {
        error = "missing required argument: --port";
        return std::nullopt;
    }
    if (options.motorIndex < 0) {
        error = "missing required argument: --index";
        return std::nullopt;
    }
    if (options.configPath.empty()) {
        error = "missing required argument: --config";
        return std::nullopt;
    }

    return options;
}

Bytes loadConfigFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open config file: " + path);
    }

    Bytes data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < ConfigSize) {
        throw std::runtime_error("config file must contain at least 48 bytes");
    }
    if (data.size() > ConfigSize) {
        logWarn("config file is larger than 48 bytes; only the first 48 bytes will be written");
        data.resize(ConfigSize);
    }

    if (data[0] != 0x01) {
        throw std::runtime_error("invalid config: byte 0 must be 0x01");
    }
    if (data[1] == 0xff || data[2] == 0x00) {
        throw std::runtime_error("invalid config: EEPROM version/bootloader bytes indicate missing settings");
    }

    return data;
}

std::uint16_t crc16FourWay(const Bytes& data, std::size_t start, std::size_t length) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < length; ++i) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(data[start + i]) << 8));
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000) != 0) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

void appendFourWayCrc(Bytes& data) {
    const std::uint16_t crc = crc16FourWay(data, 0, data.size());
    data.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xff));
    data.push_back(static_cast<std::uint8_t>(crc & 0xff));
}

Bytes makeFourWayCommand(std::uint8_t command, std::uint8_t deviceNumber) {
    Bytes out;
    out.reserve(8);
    out.push_back(FourWayPcMarker);
    out.push_back(command);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(deviceNumber);
    appendFourWayCrc(out);
    return out;
}

Bytes makeFourWayWriteCommand(const Bytes& payload, std::uint16_t address) {
    if (payload.empty() || payload.size() > 256) {
        throw std::runtime_error("four-way write payload must contain 1-256 bytes");
    }

    Bytes out;
    out.reserve(7 + payload.size());
    out.push_back(FourWayPcMarker);
    out.push_back(0x3b);
    out.push_back(static_cast<std::uint8_t>((address >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(address & 0xff));
    out.push_back(static_cast<std::uint8_t>(payload.size() == 256 ? 0 : payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    appendFourWayCrc(out);
    return out;
}

Bytes makeFourWayReadCommand(std::size_t byteCount, std::uint16_t address) {
    if (byteCount == 0 || byteCount > 256) {
        throw std::runtime_error("four-way read size must be 1-256 bytes");
    }

    Bytes out;
    out.reserve(8);
    out.push_back(FourWayPcMarker);
    out.push_back(0x3a);
    out.push_back(static_cast<std::uint8_t>((address >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(address & 0xff));
    out.push_back(0x01);
    out.push_back(static_cast<std::uint8_t>(byteCount == 256 ? 0 : byteCount));
    appendFourWayCrc(out);
    return out;
}

Bytes fourWayParams(const Bytes& frame) {
    if (frame.size() < 8) {
        return {};
    }

    const std::size_t paramCount = frame[4] == 0 ? 256 : frame[4];
    if (frame.size() < 8 + paramCount) {
        return {};
    }

    return Bytes(frame.begin() + 5, frame.begin() + static_cast<std::ptrdiff_t>(5 + paramCount));
}

Bytes makeMspCommand(std::uint8_t command, const Bytes& payload = {}) {
    if (payload.size() > 255) {
        throw std::runtime_error("MSP payload too large");
    }

    Bytes out;
    out.reserve(6 + payload.size());
    out.push_back('$');
    out.push_back('M');
    out.push_back('<');
    out.push_back(static_cast<std::uint8_t>(payload.size()));
    out.push_back(command);
    out.insert(out.end(), payload.begin(), payload.end());

    std::uint8_t checksum = 0;
    for (std::size_t i = 3; i < out.size(); ++i) {
        checksum ^= out[i];
    }
    out.push_back(checksum);
    return out;
}

std::optional<Bytes> parseMspV1ResponsePayload(const Bytes& raw, std::uint8_t expectedCommand) {
    for (std::size_t offset = 0; offset + 6 <= raw.size(); ++offset) {
        if (raw[offset] != '$' || raw[offset + 1] != 'M' || raw[offset + 2] != '>') {
            continue;
        }

        const std::size_t payloadLength = raw[offset + 3];
        const std::size_t frameLength = 6 + payloadLength;
        if (offset + frameLength > raw.size()) {
            continue;
        }

        const std::uint8_t command = raw[offset + 4];
        if (command != expectedCommand) {
            continue;
        }

        std::uint8_t checksum = 0;
        for (std::size_t i = offset + 3; i < offset + 5 + payloadLength; ++i) {
            checksum ^= raw[i];
        }

        if (checksum != raw[offset + 5 + payloadLength]) {
            continue;
        }

        return Bytes(raw.begin() + static_cast<std::ptrdiff_t>(offset + 5),
                     raw.begin() + static_cast<std::ptrdiff_t>(offset + 5 + payloadLength));
    }

    return std::nullopt;
}

bool frameHasValidCrc(const Bytes& frame) {
    if (frame.size() < 3) {
        return false;
    }
    const std::uint16_t actual = static_cast<std::uint16_t>((frame[frame.size() - 2] << 8) | frame[frame.size() - 1]);
    const std::uint16_t expected = crc16FourWay(frame, 0, frame.size() - 2);
    return actual == expected;
}

ParsedFourWayResponse parseFourWayResponse(const Bytes& raw) {
    if (raw.empty()) {
        return {{}, "no response bytes received"};
    }

    for (std::size_t offset = 0; offset + 8 <= raw.size(); ++offset) {
        if (raw[offset] != FourWayEscMarker && raw[offset] != FourWayPcMarker) {
            continue;
        }

        std::vector<std::size_t> candidateLengths;
        const std::uint8_t payloadLengthByte = raw[offset + 4];
        candidateLengths.push_back(8 + static_cast<std::size_t>(payloadLengthByte));
        if (payloadLengthByte == 0) {
            candidateLengths.push_back(8);
            candidateLengths.push_back(8 + 256);
        }

        for (const auto expectedFrameLength : candidateLengths) {
            if (expectedFrameLength < 8 || offset + expectedFrameLength > raw.size()) {
                continue;
            }

            Bytes candidate(raw.begin() + static_cast<std::ptrdiff_t>(offset),
                            raw.begin() + static_cast<std::ptrdiff_t>(offset + expectedFrameLength));
            if (frameHasValidCrc(candidate)) {
                return {candidate, {}};
            }
        }

        for (std::size_t end = offset + 8; end <= raw.size(); ++end) {
            Bytes candidate(raw.begin() + static_cast<std::ptrdiff_t>(offset),
                            raw.begin() + static_cast<std::ptrdiff_t>(end));
            if (frameHasValidCrc(candidate)) {
                return {candidate, {}};
            }
        }
    }

    return {{}, "no valid four-way frame found in response: " + hexDump(raw)};
}

bool isAckOk(const Bytes& frame) {
    return frame.size() >= 3 && frame[frame.size() - 3] == 0x00;
}

std::uint8_t ackByte(const Bytes& frame) {
    if (frame.size() < 3) {
        return 0xff;
    }
    return frame[frame.size() - 3];
}

std::string fourWayCommandName(std::uint8_t command) {
    switch (command) {
        case 0x34:
            return "interface/end";
        case 0x35:
            return "reset";
        case 0x37:
            return "connect";
        case 0x3a:
            return "read";
        case 0x3b:
            return "write";
        case 0x3d:
            return "read EEPROM";
        default:
            return "command " + hexByte(command);
    }
}

std::string fourWayAckDescription(std::uint8_t ack) {
    if (ack == 0x00) {
        return "ACK OK";
    }
    return "ACK " + hexByte(ack) + " (interface/ESC rejected the command)";
}

std::string describeFourWayFrame(const Bytes& frame) {
    if (frame.size() < 8) {
        return "short frame: " + hexDump(frame);
    }

    std::ostringstream out;
    out << "marker=" << hexByte(frame[0])
        << ", command=" << fourWayCommandName(frame[1])
        << ", length=" << static_cast<int>(frame[4])
        << ", " << fourWayAckDescription(ackByte(frame));
    return out.str();
}

std::string lastWindowsError() {
    const DWORD error = GetLastError();
    if (error == 0) {
        return {};
    }

    LPSTR buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);

    std::string message = length == 0 ? "Windows error " + std::to_string(error) : std::string(buffer, length);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
        message.pop_back();
    }
    return message;
}

class SerialPort {
public:
    SerialPort() = default;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    ~SerialPort() {
        close();
    }

    void open(const std::string& portName, DWORD baudRate) {
        const std::string deviceName = portName.rfind("\\\\.\\", 0) == 0 ? portName : "\\\\.\\" + portName;
        handle_ = CreateFileA(
            deviceName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (handle_ == INVALID_HANDLE_VALUE) {
            throw SerialIoException("failed to open " + portName + ": " + lastWindowsError());
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) {
            throw SerialIoException("GetCommState failed: " + lastWindowsError());
        }

        dcb.BaudRate = baudRate;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fTXContinueOnXoff = TRUE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;

        if (!SetCommState(handle_, &dcb)) {
            throw SerialIoException("SetCommState failed: " + lastWindowsError());
        }

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 1000;
        if (!SetCommTimeouts(handle_, &timeouts)) {
            throw SerialIoException("SetCommTimeouts failed: " + lastWindowsError());
        }

        SetupComm(handle_, 4096, 4096);
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }

    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    void writeAll(const Bytes& data) {
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw SerialIoException("serial port is not open");
        }

        std::size_t offset = 0;
        while (offset < data.size()) {
            DWORD written = 0;
            const DWORD remaining = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 4096));
            const BOOL ok = WriteFile(
                handle_,
                data.data() + offset,
                remaining,
                &written,
                nullptr);
            if (!ok || written == 0) {
                throw SerialIoException("serial write failed: " + lastWindowsError());
            }
            offset += written;
        }
    }

    Bytes readAvailable(int totalTimeoutMs, int quietTimeoutMs) {
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw SerialIoException("serial port is not open");
        }

        Bytes result;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalTimeoutMs);
        auto quietDeadline = deadline;
        bool receivedAny = false;

        while (std::chrono::steady_clock::now() < deadline) {
            DWORD errors = 0;
            COMSTAT status{};
            if (!ClearCommError(handle_, &errors, &status)) {
                throw SerialIoException("serial status read failed: " + lastWindowsError());
            }

            if (status.cbInQue > 0) {
                Bytes buffer(std::min<DWORD>(status.cbInQue, 512));
                DWORD bytesRead = 0;
                if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)) {
                    throw SerialIoException("serial read failed: " + lastWindowsError());
                }
                buffer.resize(bytesRead);
                result.insert(result.end(), buffer.begin(), buffer.end());
                receivedAny = true;
                quietDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(quietTimeoutMs);
                continue;
            }

            if (receivedAny && std::chrono::steady_clock::now() >= quietDeadline) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return result;
    }

    void drain(int timeoutMs) {
        static_cast<void>(readAvailable(timeoutMs, 25));
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

ParsedFourWayResponse transactFourWay(SerialPort& serial,
                                      const Bytes& request,
                                      const std::string& operation,
                                      int readTimeoutMs = 1000) {
    logInfo(operation + ": TX " + hexDump(request));
    serial.writeAll(request);
    const Bytes raw = serial.readAvailable(readTimeoutMs, 75);
    logInfo(operation + ": RX " + (raw.empty() ? std::string("<none>") : hexDump(raw)));
    return parseFourWayResponse(raw);
}

std::uint16_t connectEsc(SerialPort& serial, int motorIndex, int maxAttempts, int retryDelayMs) {
    std::string lastFailure;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        logInfo("connecting to ESC index " + std::to_string(motorIndex) + ", attempt " +
                std::to_string(attempt) + "/" + std::to_string(maxAttempts));

        auto response = transactFourWay(
            serial,
            makeFourWayCommand(0x37, static_cast<std::uint8_t>(motorIndex)),
            "four-way connect",
            1000);

        if (!response.error.empty()) {
            lastFailure = response.error;
            logInfo("ESC connect attempt failed: " + lastFailure);
        } else if (!isAckOk(response.frame)) {
            lastFailure = describeFourWayFrame(response.frame);
            logInfo("ESC connect attempt rejected: " + lastFailure);
        } else {
            if (response.frame.size() <= 6) {
                throw std::runtime_error("ESC connect frame is too short to identify MCU");
            }

            const std::uint8_t mcu = response.frame[6];
            if (mcu == 0x2b) {
                logInfo("detected G071 ESC, EEPROM address " + hexWord(EepromAddressG071));
                return EepromAddressG071;
            }
            if (mcu == 0x1f) {
                logInfo("detected F051 ESC, EEPROM address " + hexWord(EepromAddressF051));
                return EepromAddressF051;
            }
            if (mcu == 0x35) {
                logInfo("detected F3 ESC, EEPROM address " + hexWord(EepromAddressF3));
                return EepromAddressF3;
            }
            if (mcu == 0x15) {
                logInfo("detected NXP ESC, EEPROM address " + hexWord(EepromAddressNxp));
                return EepromAddressNxp;
            }

            lastFailure = "unsupported/unknown ESC MCU id " + hexByte(mcu);
            logInfo("ESC connect attempt failed: " + lastFailure);
        }

        if (attempt < maxAttempts && retryDelayMs > 0) {
            logInfo("waiting " + std::to_string(retryDelayMs) + " ms before next ESC connect attempt");
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
    }

    throw std::runtime_error("could not connect to ESC index " + std::to_string(motorIndex) +
                             ". Last response: " + (lastFailure.empty() ? "none" : lastFailure));
}

std::optional<int> startPassthrough(SerialPort& serial, int settleDelayMs) {
    logInfo("starting MSP/four-way passthrough");

    const Bytes enableMotorControl = makeMspCommand(0x68);
    logInfo("MSP motor-control enable: TX " + hexDump(enableMotorControl));
    serial.writeAll(enableMotorControl);
    const Bytes enableResponse = serial.readAvailable(300, 50);
    logInfo("MSP motor-control enable: RX " + (enableResponse.empty() ? std::string("<none>") : hexDump(enableResponse)));

    const Bytes startFourWay = makeMspCommand(0xf5);
    logInfo("MSP four-way passthrough: TX " + hexDump(startFourWay));
    serial.writeAll(startFourWay);
    const Bytes passthroughResponse = serial.readAvailable(300, 50);
    logInfo("MSP four-way passthrough: RX " + (passthroughResponse.empty() ? std::string("<none>") : hexDump(passthroughResponse)));

    std::optional<int> expectedEscCount;
    if (auto payload = parseMspV1ResponsePayload(passthroughResponse, 0xf5); payload && !payload->empty()) {
        expectedEscCount = static_cast<int>((*payload)[0]);
        logInfo("flight controller reports " + std::to_string(*expectedEscCount) + " ESC output(s)");
    }

    if (settleDelayMs > 0) {
        logInfo("waiting " + std::to_string(settleDelayMs) + " ms for four-way passthrough to settle");
        std::this_thread::sleep_for(std::chrono::milliseconds(settleDelayMs));
    }

    return expectedEscCount;
}

void writeEeprom(SerialPort& serial, const Bytes& config, std::uint16_t eepromAddress) {
    auto response = transactFourWay(
        serial,
        makeFourWayWriteCommand(config, eepromAddress),
        "EEPROM write",
        1500);

    if (!response.error.empty()) {
        throw ProtocolException(response.error);
    }
    if (!isAckOk(response.frame)) {
        throw std::runtime_error("ESC returned bad ACK for EEPROM write: " + describeFourWayFrame(response.frame));
    }

    logInfo("EEPROM write acknowledged by ESC");
}

void verifyEepromReadback(SerialPort& serial, const Bytes& expectedConfig, std::uint16_t eepromAddress) {
    auto response = transactFourWay(
        serial,
        makeFourWayReadCommand(expectedConfig.size(), eepromAddress),
        "EEPROM readback",
        1500);

    if (!response.error.empty()) {
        throw ProtocolException(response.error);
    }
    if (!isAckOk(response.frame)) {
        throw std::runtime_error("ESC returned bad ACK for EEPROM readback: " + describeFourWayFrame(response.frame));
    }

    const Bytes actual = fourWayParams(response.frame);
    if (actual.size() != expectedConfig.size()) {
        throw std::runtime_error("EEPROM readback size mismatch: expected " +
                                 std::to_string(expectedConfig.size()) + ", got " + std::to_string(actual.size()));
    }

    if (actual != expectedConfig) {
        for (std::size_t i = 0; i < expectedConfig.size(); ++i) {
            if (actual[i] != expectedConfig[i]) {
                throw std::runtime_error("EEPROM readback mismatch at byte " + std::to_string(i) +
                                         ": expected " + hexByte(expectedConfig[i]) +
                                         ", got " + hexByte(actual[i]));
            }
        }
    }

    logInfo("EEPROM readback matches written config");
}

void cleanupPassthrough(SerialPort& serial, bool resetFlightController) {
    try {
        logInfo("ending four-way interface");
        auto endResponse = transactFourWay(serial, makeFourWayCommand(0x34, 0x00), "four-way end", 500);
        if (!endResponse.error.empty()) {
            logWarn("four-way end response was not valid: " + endResponse.error);
        }
    } catch (const std::exception& ex) {
        logWarn(std::string("four-way cleanup failed: ") + ex.what());
    }

    if (!resetFlightController) {
        logInfo("FC reset skipped");
        return;
    }

    try {
        const Bytes resetFc = makeMspCommand(0x44);
        logInfo("MSP FC reset: TX " + hexDump(resetFc));
        serial.writeAll(resetFc);
        logInfo("MSP FC reset sent; no response is required");
    } catch (const std::exception& ex) {
        logWarn(std::string("MSP cleanup failed: ") + ex.what());
    }
}

void cleanupAfterWrite(SerialPort& serial, int motorIndex, bool resetEsc, bool resetFlightController) {
    if (resetEsc) {
        try {
            logInfo("resetting ESC after write");
            auto resetResponse = transactFourWay(
                serial,
                makeFourWayCommand(0x35, static_cast<std::uint8_t>(motorIndex)),
                "ESC reset",
                500);
            if (!resetResponse.error.empty()) {
                logWarn("ESC reset response was not valid: " + resetResponse.error);
            }
        } catch (const std::exception& ex) {
            logWarn(std::string("ESC reset cleanup failed: ") + ex.what());
        }
    } else {
        logInfo("ESC reset skipped");
    }

    cleanupPassthrough(serial, resetFlightController);
}

int run(const Options& options) {
    Bytes config;
    try {
        config = loadConfigFile(options.configPath);
    } catch (const std::exception& ex) {
        logError(ex.what());
        return static_cast<int>(ExitCode::ConfigFileError);
    }

    logInfo("loaded config file: " + options.configPath);
    logInfo("config bytes to write: " + std::to_string(config.size()));

    if (options.reverse) {
        config[DirectionByteIndex] = 0x01;
        logInfo("reverse requested: EEPROM byte 17 set to 1");
    } else {
        logInfo("reverse not requested: EEPROM byte 17 left as " + hexByte(config[DirectionByteIndex]));
    }

    SerialPort serial;
    try {
        logInfo("opening serial port " + options.port + " at 115200 8N1");
        serial.open(options.port, CBR_115200);
    } catch (const SerialIoException& ex) {
        logError(ex.what());
        return static_cast<int>(ExitCode::SerialOpenError);
    }

    std::optional<int> expectedEscCount;
    try {
        expectedEscCount = startPassthrough(serial, options.passthroughDelayMs);
    } catch (const SerialIoException& ex) {
        logError(std::string("serial I/O failed while starting passthrough: ") + ex.what());
        return static_cast<int>(ExitCode::SerialIoError);
    } catch (const std::exception& ex) {
        logError(std::string("failed to start passthrough: ") + ex.what());
        return static_cast<int>(ExitCode::PassthroughError);
    }

    if (expectedEscCount && options.motorIndex >= *expectedEscCount) {
        logError("requested ESC index " + std::to_string(options.motorIndex) +
                 " but flight controller reported only " + std::to_string(*expectedEscCount) + " ESC output(s)");
        cleanupPassthrough(serial, false);
        return static_cast<int>(ExitCode::InvalidArguments);
    }

    std::uint16_t eepromAddress = 0;
    try {
        logInfo("ESC connect retry policy: " + std::to_string(options.connectAttempts) +
                " attempts, " + std::to_string(options.connectDelayMs) + " ms delay");
        eepromAddress = connectEsc(serial, options.motorIndex, options.connectAttempts, options.connectDelayMs);
    } catch (const SerialIoException& ex) {
        logError(std::string("serial I/O failed while connecting to ESC: ") + ex.what());
        return static_cast<int>(ExitCode::SerialIoError);
    } catch (const std::exception& ex) {
        logError(ex.what());
        cleanupPassthrough(serial, false);
        return static_cast<int>(ExitCode::EscConnectError);
    }

    try {
        logInfo("writing 48-byte EEPROM config to address " + hexWord(eepromAddress));
        writeEeprom(serial, config, eepromAddress);
        if (options.verifyReadback) {
            verifyEepromReadback(serial, config, eepromAddress);
        } else {
            logInfo("EEPROM readback verification skipped");
        }
    } catch (const SerialIoException& ex) {
        logError(std::string("serial I/O failed during EEPROM write: ") + ex.what());
        cleanupPassthrough(serial, false);
        return static_cast<int>(ExitCode::SerialIoError);
    } catch (const ProtocolException& ex) {
        logError(std::string("protocol error during EEPROM write: ") + ex.what());
        cleanupPassthrough(serial, false);
        return static_cast<int>(ExitCode::ProtocolError);
    } catch (const std::exception& ex) {
        logError(std::string("EEPROM write failed: ") + ex.what());
        cleanupPassthrough(serial, false);
        return static_cast<int>(ExitCode::EscWriteError);
    }

    cleanupAfterWrite(serial, options.motorIndex, options.resetEscAfterWrite, options.resetFcAfterWrite);
    logInfo("completed successfully");
    return static_cast<int>(ExitCode::Success);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string parseError;
    auto options = parseArguments(argc, argv, parseError);

    if (!options) {
        logError(parseError);
        printUsage();
        return static_cast<int>(ExitCode::InvalidArguments);
    }

    if (options->help) {
        printUsage();
        return static_cast<int>(ExitCode::Success);
    }

    return run(*options);
}
