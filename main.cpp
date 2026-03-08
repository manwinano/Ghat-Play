#include <SFML/Network.hpp>
#include <iostream>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <optional>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

#ifdef _WIN32
// Подавляем все предупреждения об устаревших функциях Winsock
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

// Для старых версий Windows
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 22
#endif

#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

class ChatServer {
private:
    sf::TcpListener listener;
    std::vector<std::unique_ptr<sf::TcpSocket>> clients;
    std::atomic<bool> isRunning{ true };
    const unsigned short PORT = 53000;

    // Вспомогательная функция для отправки всех данных
    bool sendAll(sf::TcpSocket& socket, const void* data, std::size_t size) {
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

    // Отправка сообщения с префиксом длины
    bool sendMessage(sf::TcpSocket& socket, const std::string& message) {
        uint32_t messageLength = static_cast<uint32_t>(message.length() + 1); // +1 для null terminator
        uint32_t networkLength = htonl(messageLength);

        if (!sendAll(socket, &networkLength, sizeof(networkLength))) {
            return false;
        }

        return sendAll(socket, message.c_str(), messageLength);
    }

    // Получение сообщения с префиксом длины
    std::optional<std::string> receiveMessage(sf::TcpSocket& socket) {
        uint32_t networkLength;
        std::size_t received = 0;
        char* lengthBuffer = reinterpret_cast<char*>(&networkLength);

        // Читаем длину (4 байта)
        while (received < sizeof(networkLength)) {
            std::size_t currentReceived = 0;
            sf::Socket::Status status = socket.receive(
                lengthBuffer + received,
                sizeof(networkLength) - received,
                currentReceived
            );

            if (status == sf::Socket::Status::NotReady) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (status != sf::Socket::Status::Done) {
                return std::nullopt;
            }

            if (currentReceived == 0) {
                return std::nullopt; // Соединение закрыто
            }

            received += currentReceived;
        }

        uint32_t messageLength = ntohl(networkLength);

        if (messageLength > 65536 || messageLength == 0) {
            std::cerr << "Invalid message length: " << messageLength << std::endl;
            return std::nullopt;
        }

        // Читаем само сообщение
        std::vector<char> buffer(messageLength);
        received = 0;

        while (received < messageLength) {
            std::size_t currentReceived = 0;
            sf::Socket::Status status = socket.receive(
                buffer.data() + received,
                messageLength - received,
                currentReceived
            );

            if (status == sf::Socket::Status::NotReady) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (status != sf::Socket::Status::Done) {
                return std::nullopt;
            }

            if (currentReceived == 0) {
                return std::nullopt; // Соединение закрыто
            }

            received += currentReceived;
        }

        return std::string(buffer.data());
    }

    // Ретрансляция сообщения всем клиентам кроме отправителя
    void broadcastMessage(const std::string& message, sf::TcpSocket* sender) {
        std::cout << "Broadcasting: " << message << std::endl;

        for (const auto& client : clients) {
            if (client.get() != sender) {
                if (!sendMessage(*client, message)) {
                    std::cerr << "Failed to send to a client" << std::endl;
                }
            }
        }
    }

    // Получение текущего времени в формате [HH:MM:SS]
    std::string getCurrentTime() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        struct tm timeinfo;

#ifdef _WIN32
        localtime_s(&timeinfo, &time_t);
#else
        localtime_r(&time_t, &timeinfo);
#endif

        std::stringstream ss;
        ss << std::setw(2) << std::setfill('0') << timeinfo.tm_hour << ":"
            << std::setw(2) << std::setfill('0') << timeinfo.tm_min << ":"
            << std::setw(2) << std::setfill('0') << timeinfo.tm_sec;
        return ss.str();
    }

    // Функция для получения всех локальных IP-адресов (упрощенная версия)
    std::vector<std::string> getAllLocalIPAddresses() {
        std::vector<std::string> addresses;

#ifdef _WIN32
        // Windows implementation
        DWORD size = 0;
        GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &size);

        std::vector<BYTE> buffer(size);
        PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

        if (GetAdaptersAddresses(AF_INET, 0, NULL, adapters, &size) == ERROR_SUCCESS) {
            for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp) continue;

                for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
                    unicast; unicast = unicast->Next) {

                    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);

                    // Используем inet_ntoa (с подавленными предупреждениями)
                    char* ipStr = inet_ntoa(addr->sin_addr);

                    if (ipStr && strcmp(ipStr, "127.0.0.1") != 0) {
                        addresses.push_back(ipStr);
                    }
                }
            }
        }
#else
        // Linux/Unix implementation
        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) == 0) {
            for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
                if (ifa->ifa_addr == nullptr) continue;

                if (ifa->ifa_addr->sa_family == AF_INET) {
                    char ip[INET_ADDRSTRLEN];
                    sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
                    inet_ntop(AF_INET, &(ipv4->sin_addr), ip, INET_ADDRSTRLEN);

                    if (strcmp(ip, "127.0.0.1") != 0) {
                        addresses.push_back(ip);
                    }
                }
            }
            freeifaddrs(ifaddr);
        }
#endif

        return addresses;
    }

    // Получение строкового представления удаленного адреса клиента
    std::string getClientAddress(sf::TcpSocket* client) {
        auto remoteAddress = client->getRemoteAddress();
        if (remoteAddress.has_value()) {
            return remoteAddress->toString();
        }
        return "Unknown";
    }

public:
    bool start() {
        if (listener.listen(PORT, sf::IpAddress::Any) != sf::Socket::Status::Done) {
            std::cerr << "Failed to listen on port " << PORT << std::endl;
            return false;
        }

        std::cout << "\n=========================================" << std::endl;
        std::cout << "     SFML 3.0.2 Chat Server Started     " << std::endl;
        std::cout << "=========================================" << std::endl;
        std::cout << "Port: " << PORT << std::endl;

        // Получаем основной IP через SFML
        auto localIP = sf::IpAddress::getLocalAddress();
        if (localIP.has_value()) {
            std::cout << "Main IP: " << localIP->toString() << std::endl;
        }

        // Получаем все доступные IP адреса
        std::vector<std::string> addresses = getAllLocalIPAddresses();
        if (!addresses.empty()) {
            std::cout << "\nAvailable IPs for clients:" << std::endl;
            for (const auto& ip : addresses) {
                std::cout << "  - " << ip << std::endl;
            }
        }

        std::cout << "\nLocal connection (same computer): 127.0.0.1:" << PORT << std::endl;
        if (!addresses.empty()) {
            std::cout << "Remote connection (other computers): " << addresses[0] << ":" << PORT << std::endl;
        }

        std::cout << "=========================================\n" << std::endl;

        return true;
    }

    void run() {
        sf::SocketSelector selector;
        selector.add(listener);

        std::cout << "Server is running. Waiting for connections..." << std::endl;

        while (isRunning) {
            if (!selector.wait(sf::milliseconds(100))) {
                continue;
            }

            // Проверяем новые подключения
            if (selector.isReady(listener)) {
                auto client = std::make_unique<sf::TcpSocket>();

                if (listener.accept(*client) == sf::Socket::Status::Done) {
                    std::string clientInfo = getClientAddress(client.get());

                    std::cout << "[" << getCurrentTime() << "] New client connected: " << clientInfo
                        << ":" << client->getRemotePort() << std::endl;

                    client->setBlocking(false);
                    selector.add(*client);
                    clients.push_back(std::move(client));

                    // Отправляем приветствие новому клиенту
                    std::string welcomeMsg = "System: Welcome to the chat! There are " +
                        std::to_string(clients.size()) + " user" + (clients.size() > 1 ? "s" : "") + " online.";
                    sendMessage(*clients.back(), welcomeMsg);

                    // Уведомляем остальных о новом пользователе
                    std::string notifyMsg = "System: New user joined! (" +
                        std::to_string(clients.size()) + " user" + (clients.size() > 1 ? "s" : "") + " online)";
                    broadcastMessage(notifyMsg, clients.back().get());
                }
            }

            // Проверяем сообщения от всех клиентов
            for (auto it = clients.begin(); it != clients.end();) {
                sf::TcpSocket* client = it->get();

                if (selector.isReady(*client)) {
                    auto message = receiveMessage(*client);

                    if (message.has_value()) {
                        // Получили сообщение - ретранслируем всем
                        std::cout << "[" << getCurrentTime() << "] " << message.value() << std::endl;
                        broadcastMessage(message.value(), client);
                        ++it;
                    }
                    else {
                        // Клиент отключился или ошибка
                        std::string clientInfo = getClientAddress(client);

                        std::cout << "[" << getCurrentTime() << "] Client disconnected: " << clientInfo << std::endl;

                        selector.remove(*client);
                        it = clients.erase(it);

                        // Уведомляем остальных об отключении
                        if (!clients.empty()) {
                            std::string notifyMsg = "System: A user left. (" +
                                std::to_string(clients.size()) + " user" + (clients.size() > 1 ? "s" : "") + " online)";
                            broadcastMessage(notifyMsg, nullptr);
                        }
                    }
                }
                else {
                    ++it;
                }
            }
        }
    }

    void stop() {
        isRunning = false;
    }
};

#ifdef _WIN32
int main() {
    // Подавляем предупреждения о неиспользуемых параметрах
#pragma warning(disable: 4100)

// Инициализация Winsock на Windows
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Failed to initialize Winsock" << std::endl;
        return -1;
    }
#else
int main() {
#endif

    ChatServer server;

    if (!server.start()) {
        std::cout << "\nPress Enter to exit..." << std::endl;
        std::cin.get();

#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    std::cout << "Server running. Press Enter to stop..." << std::endl;

    std::thread serverThread([&server]() {
        server.run();
        });

    std::cin.get();

    std::cout << "\nStopping server..." << std::endl;
    server.stop();
    serverThread.join();

    std::cout << "Server stopped." << std::endl;

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}