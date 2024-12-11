#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>

#include <cstdint>
#include <poll.h>
#include <unistd.h>

#include <inetserverdgram.hpp>
#include <tuntap++.hh>
#include <unordered_set>

constexpr bool debug = false;

class MultiTun {
    using string_pair = std::pair<std::string, std::string>;
    struct Endpoint : public string_pair {
        struct Hasher {
            inline std::size_t operator()(const Endpoint& endpoint) const {
                return std::hash<std::string>{}(endpoint.first + endpoint.second);
            }
        };

        std::string& addr() { return first; }
        std::string& port() { return second; }
        const std::string& addr() const { return first; }
        const std::string& port() const { return second; }

        Endpoint() : string_pair{{}, {}} {}

        Endpoint(const std::string& addr, const std::string& port) : string_pair{addr, port} {}
    };

    static constexpr int mtu = 1500;

    // socket data
    std::unique_ptr<tuntap::tun> tun_sock;
    std::unique_ptr<libsocket::inet_dgram_server> udp_sock;
    std::array<struct pollfd, 2> fds{};
    struct pollfd& tun_fd = fds[0];
    struct pollfd& udp_fd = fds[1];

    // control data
    std::unordered_set<Endpoint, Endpoint::Hasher> endpoints;

public:
    // config data
    std::string tun_listen_addr;
    std::string udp_listen_addr;
    std::string udp_listen_port;

    MultiTun() {}

    ~MultiTun() {}

    void init() {
        tun_sock = std::make_unique<tuntap::tun>();
        tun_sock->ip(tun_listen_addr, 24);
        tun_sock->mtu(mtu);
        tun_sock->up();
        udp_sock = std::make_unique<libsocket::inet_dgram_server>(udp_listen_addr, udp_listen_port,
                                                                  LIBSOCKET_IPv4);
        std::memset(fds.data(), 0, sizeof(*fds.data()));
        tun_fd.events = POLLIN;
        udp_fd.events = POLLIN;
        tun_fd.fd = tun_sock->native_handle();
        udp_fd.fd = udp_sock->getfd();
    }

    void add_endpoint(const std::string& addr, const std::string& port) {
        endpoints.insert(Endpoint{addr, port});
        for (const auto& endpoint: endpoints) {
            std::cout << "manually adding endpoint " << endpoint.addr() << ":" << endpoint.port()
                      << std::endl;
        }
    }

    void run_loop() {
        Endpoint endpoint;
        std::array<uint8_t, mtu> buffer;
        int size;
        while (true) {
            if (debug) std::cout << "polling ..." << std::endl;
            int poll_res = poll(fds.data(), 2, -1);
            if (poll_res < 0) { break; }
            if (debug) std::cout << "poll() returned " << poll_res << std::endl;
            if (tun_fd.revents & POLLIN) {
                size = tun_sock->read(buffer.data(), mtu);
                if (debug) std::cout << "got tun packet of size " << size << std::endl;
                for (const auto& endpoint: endpoints) {
                    if (debug)
                        std::cout << "sending to endpoint " << endpoint.addr() << ":"
                                  << endpoint.port() << std::endl;
                    udp_sock->sndto(buffer.data(), size, endpoint.addr(), endpoint.port());
                }
            }
            if (udp_fd.revents & POLLIN) {
                size = udp_sock->rcvfrom(buffer.data(), mtu, endpoint.addr(), endpoint.port(), 0,
                                         true);
                if (debug) std::cout << "got udp packet of size " << size << std::endl;
                bool inserted = endpoints.insert(endpoint).second;
                if (inserted) {
                    std::cout << "automatically added endpoint " << endpoint.addr() << ":"
                              << endpoint.port() << std::endl;
                }
                tun_sock->write(buffer.data(), size);
            }
            if (debug) std::cout << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        MultiTun multi_tun;
        multi_tun.udp_listen_port = "4242";
        if (argc == 3 && std::strcmp(argv[1], "server") == 0) {
            multi_tun.udp_listen_addr = argv[2];
            std::cout << "acting as server, bound to " << multi_tun.udp_listen_addr << std::endl;
            // .1 is server
            multi_tun.tun_listen_addr = "10.42.42.1";
        } else if (argc == 4 && std::strcmp(argv[1], "client") == 0) {
            multi_tun.udp_listen_addr = argv[2];
            const std::string server_addr = argv[3];
            std::cout << "acting as client, bound to " << multi_tun.udp_listen_addr
                      << ", connecting to server " << server_addr << std::endl;
            // .2 is client
            multi_tun.tun_listen_addr = "10.42.42.2";
            if (argc > 1) { multi_tun.add_endpoint(server_addr, "4242"); }
        } else {
            std::cout << "usage: " << argv[0] << " <client|server> <bind addr> [server addr]"
                      << std::endl;
            return -1;
        }
        multi_tun.init();
        multi_tun.run_loop();
    } catch (libsocket::socket_exception& e) { std::cerr << "error: " << e.mesg << std::endl; }
}