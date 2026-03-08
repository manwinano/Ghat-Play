#include <SFML/Network.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <optional>
#include <vector>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif

class ChatClient {
private:
    sf::TcpSocket socket;
    std::atomic<bool> isRunning{ true };
    std::string username;

    // Вспомогательная функция для отправки всех данных
    bool sendAll(const void* data, std::size_t size) {
        const char* buffer = static_cast<const char*>(data);
        std::size_t sent = 0;

        while (sent < size) {
            std::size_t currentSent = 0;
            sf::Socket::Status status = socket.send(buffer + sent, size - sent, currentSent);

            if (status != sf::Socket::Status::Done) {
                return false;
            }

            if (currentSent == 0) {
                return false; // Соединение закрыто
            }

            sent += currentSent;
        }
        return true;
    }

    // Вспомогательная функция для получения всех данных
    bool receiveAll(void* data, std::size_t size) {
        char* buffer = static_cast<char*>(data);
        std::size_t received = 0;

        while (received < size) {
            std::size_t currentReceived = 0;
            sf::Socket::Status status = socket.receive(buffer + received, size - received, currentReceived);

            if (status == sf::Socket::Status::NotReady) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (status != sf::Socket::Status::Done) {
                return false;
            }

            if (currentReceived == 0) {
                return false; // Соединение закрыто
            }

            received += currentReceived;
        }
        return true;
    }

    // Отправка сообщения с префиксом длины
    bool sendMessage(const std::string& message) {
        uint32_t messageLength = static_cast<uint32_t>(message.length() + 1);
        uint32_t networkLength = htonl(messageLength);

        if (!sendAll(&networkLength, sizeof(networkLength))) {
            return false;
        }

        return sendAll(message.c_str(), messageLength);
    }

    // Получение сообщения с префиксом длины
    std::optional<std::string> receiveMessage() {
        uint32_t networkLength;
        if (!receiveAll(&networkLength, sizeof(networkLength))) {
            return std::nullopt;
        }

        uint32_t messageLength = ntohl(networkLength);

        if (messageLength > 65536 || messageLength == 0) {
            std::cerr << "\n! Invalid message length: " << messageLength << std::endl;
            return std::nullopt;
        }

        std::vector<char> buffer(messageLength);
        if (!receiveAll(buffer.data(), messageLength)) {
            return std::nullopt;
        }

        return std::string(buffer.data());
    }

public:
    bool connect(const std::string& ip, unsigned short port) {
        std::optional<sf::IpAddress> serverIP = sf::IpAddress::resolve(ip);

        if (!serverIP.has_value()) {
            std::cerr << "Invalid IP address: " << ip << std::endl;
            return false;
        }

        sf::Time timeout = sf::seconds(5);
        std::cout << "Connecting to " << ip << ":" << port << "..." << std::endl;

        sf::Socket::Status status = socket.connect(serverIP.value(), port, timeout);

        if (status != sf::Socket::Status::Done) {
            std::cerr << "Failed to connect to server. ";
            if (status == sf::Socket::Status::NotReady) {
                std::cerr << "Connection timeout.";
            }
            else if (status == sf::Socket::Status::Disconnected) {
                std::cerr << "Server refused connection.";
            }
            else {
                std::cerr << "Unknown error.";
            }
            std::cerr << std::endl;
            return false;
        }

        socket.setBlocking(false);
        std::cout << "✓ Connected to server!" << std::endl;

        auto remoteAddress = socket.getRemoteAddress();
        if (remoteAddress.has_value()) {
            std::cout << "Server address: " << remoteAddress->toString()
                << ":" << socket.getRemotePort() << std::endl;
        }

        return true;
    }

    void setUsername(const std::string& name) {
        username = name;
    }

    void run() {
        std::cout << "\n=========================================" << std::endl;
        std::cout << "      SFML 3.0.2 Chat Client Started     " << std::endl;
        std::cout << "=========================================" << std::endl;
        std::cout << "Username: " << username << std::endl;
        std::cout << "Type '/quit' to exit, '/help' for commands." << std::endl;
        std::cout << "=========================================\n" << std::endl;

        std::thread receiveThread(&ChatClient::receiveMessages, this);

        std::string message;
        while (isRunning) {
            std::cout << username << "> ";
            std::getline(std::cin, message);

            if (!isRunning) break;

            if (message == "/quit") {
                std::cout << "Disconnecting from server..." << std::endl;
                break;
            }
            else if (message == "/help") {
                showHelp();
                continue;
            }
            else if (message.empty()) {
                continue;
            }

            std::string formatted = username + ": " + message;

            if (!sendMessage(formatted)) {
                std::cerr << "\n! Failed to send message. Connection lost." << std::endl;
                isRunning = false;
                break;
            }
        }

        isRunning = false;
        socket.disconnect();

        if (receiveThread.joinable()) {
            receiveThread.join();
        }
    }

private:
    void receiveMessages() {
        while (isRunning) {
            auto message = receiveMessage();

            if (message.has_value()) {
                // Очищаем текущую строку и выводим сообщение
                std::cout << "\r" << std::string(80, ' ') << "\r";
                std::cout << message.value() << std::endl;
                std::cout << username << "> " << std::flush;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void showHelp() {
        std::cout << "\n=== Available Commands ===" << std::endl;
        std::cout << "/quit  - Disconnect and exit" << std::endl;
        std::cout << "/help  - Show this help message" << std::endl;
        std::cout << "Any other text will be sent as a message" << std::endl;
        std::cout << "=========================\n" << std::endl;
    }
};

#ifdef _WIN32
int main() {
    // Инициализация Winsock на Windows
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Failed to initialize Winsock" << std::endl;
        return -1;
    }
#else
int main() {
#endif

    try {
        ChatClient client;

        std::cout << "=== SFML 3.0.2 Chat Client ===" << std::endl;

        std::string serverIp;
        std::cout << "Enter server IP [127.0.0.1]: ";
        std::getline(std::cin, serverIp);
        if (serverIp.empty()) serverIp = "127.0.0.1";

        std::string portStr;
        unsigned short port = 53000;
        std::cout << "Enter server port [53000]: ";
        std::getline(std::cin, portStr);
        if (!portStr.empty()) {
            try {
                port = static_cast<unsigned short>(std::stoi(portStr));
            }
            catch (...) {
                std::cout << "Invalid port, using 53000" << std::endl;
            }
        }

        std::string username;
        std::cout << "Enter your username: ";
        std::getline(std::cin, username);
        while (username.empty()) {
            std::cout << "Username cannot be empty. Enter your username: ";
            std::getline(std::cin, username);
        }
        client.setUsername(username);

        if (!client.connect(serverIp, port)) {
            std::cout << "\nPress Enter to exit..." << std::endl;
            std::cin.get();

#ifdef _WIN32
            WSACleanup();
#endif
            return -1;
        }

        client.run();

        std::cout << "\nClient stopped. Press Enter to exit..." << std::endl;
        std::cin.get();
    }
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        std::cout << "\nPress Enter to exit..." << std::endl;
        std::cin.get();

#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}