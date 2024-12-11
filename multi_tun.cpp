#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include <cstdint>
#include <poll.h>
#include <unistd.h>

#include <inetclientdgram.hpp>
#include <inetserverdgram.hpp>
#include <tuntap++.hh>

constexpr bool debug = false;

class MultiTun {
public:
    using UdpSocket = libsocket::inet_dgram_server;

    struct Endpoint {
        std::string addr;
        std::string port;
        std::shared_ptr<UdpSocket> udp_sock;

        std::string get_key() const { return addr + ':' + port; }
    };

protected:
    static constexpr int mtu = 1500;
    static constexpr int max_socks = 100;
    static constexpr int max_stored_packets = 1024;

    // socket data
    std::unique_ptr<tuntap::tun> tun_sock;
    std::shared_ptr<UdpSocket> server_udp_sock;
    std::array<struct pollfd, max_socks> fds;
    int n_udp_fds = 0;

    // control data
    std::unordered_map<std::string, Endpoint> endpoints;
    std::unordered_map<int, std::shared_ptr<UdpSocket>> fd_to_sock;

    // deduplication data
    std::array<std::array<uint8_t, mtu>, max_stored_packets> packet_list;
    int packet_cnt = 0;

    void init() {
        if (tun_sock) { throw std::runtime_error("double init"); }
        tun_sock = std::make_unique<tuntap::tun>();
        tun_sock->ip(tun_listen_addr, 24);
        tun_sock->mtu(mtu);
        tun_sock->up();
        std::memset(fds.data(), 0, sizeof(*fds.data()));
        n_udp_fds = 0;
        packet_cnt = 0;
    }

public:
    // config data
    std::string tun_listen_addr;
    std::string udp_listen_port;

    MultiTun() {}

    ~MultiTun() {}

    void init_server(const std::string& udp_listen_addr) {
        init();

        // server socket/endpoint
        server_udp_sock =
                std::make_shared<UdpSocket>(udp_listen_addr, udp_listen_port, LIBSOCKET_IPv4);

        struct pollfd& udp_fd = fds[++n_udp_fds];
        udp_fd.fd = server_udp_sock->getfd();
        udp_fd.events = POLLIN;
        fd_to_sock.insert({udp_fd.fd, server_udp_sock});
    }

    void init_client() { init(); }

    void add_endpoint(const std::string& udp_listen_addr, const std::string& server_addr) {
        const auto udp_sock = std::make_shared<UdpSocket>(udp_listen_addr, "", LIBSOCKET_IPv4);
        // udp_sock->connect(server_addr, udp_listen_port);
        MultiTun::Endpoint new_ep{server_addr, udp_listen_port, udp_sock};
        std::cout << "manually adding endpoint " << new_ep.get_key() << std::endl;
        endpoints[new_ep.get_key()] = std::move(new_ep);

        struct pollfd& udp_fd = fds[++n_udp_fds];
        udp_fd.fd = udp_sock->getfd();
        udp_fd.events = POLLIN;
        fd_to_sock.insert({udp_fd.fd, udp_sock});
    }

    void run_loop() {
        struct pollfd& tun_fd = fds[0];
        tun_fd.fd = tun_sock->native_handle();
        tun_fd.events = POLLIN;

        std::array<uint8_t, mtu> buffer;
        int size;
        while (true) {
            if (debug) std::cout << "polling on " << 1 + n_udp_fds << " fds ..." << std::endl;
            int poll_res = poll(fds.data(), 1 + n_udp_fds, -1);
            if (poll_res < 0) { break; }
            if (debug) std::cout << "poll() returned " << poll_res << std::endl;
            if (tun_fd.revents & POLLIN) {
                size = tun_sock->read(buffer.data(), mtu);
                if (debug) std::cout << "got tun packet of size " << size << std::endl;
                for (const auto& [_, ep]: endpoints) {
                    if (debug) std::cout << "sending to endpoint " << ep.get_key() << std::endl;
                    ep.udp_sock->sndto(buffer.data(), size, ep.addr, ep.port);
                }
            }
            for (int i = 0; i < n_udp_fds; ++i) {
                auto& fd = fds[i + 1];
                if ((fd.revents & POLLIN) == 0) { continue; }
                std::shared_ptr<UdpSocket> udp_sock = fd_to_sock.at(fd.fd);
                Endpoint tmp_ep;
                size = udp_sock->rcvfrom(buffer.data(), mtu, tmp_ep.addr, tmp_ep.port, 0, true);
                if (debug) std::cout << "got udp packet of size " << size << std::endl;
                if (size <= 0) { throw std::runtime_error("invalid udp packet size"); }

                // deduplicate
                if (size < mtu) { std::memset(&buffer.at(size), 0, mtu - size); }
                bool is_duplicate = false;
                for (int i = packet_cnt; i >= 0 && i >= packet_cnt - max_stored_packets; --i) {
                    if (buffer == packet_list[i % max_stored_packets]) {
                        is_duplicate = true;
                        break;
                    }
                }
                if (!is_duplicate) {
                    // send to tun
                    tun_sock->write(buffer.data(), size);
                    packet_list[++packet_cnt % max_stored_packets] = buffer;
                }

                if (udp_sock == server_udp_sock && 1 + n_udp_fds < max_socks) {
                    // try to add new client
                    auto [it, inserted] = endpoints.insert({tmp_ep.get_key(), tmp_ep});
                    if (inserted) {
                        Endpoint& new_ep = it->second;
                        std::cout << "automatically added endpoint " << new_ep.get_key()
                                  << std::endl;
                        new_ep.udp_sock = server_udp_sock;
                    }
                }
            }
            if (debug) std::cout << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        MultiTun multi_tun;
        multi_tun.udp_listen_port = "5678";
        if (argc == 3 && std::strcmp(argv[1], "server") == 0) {
            const std::string udp_listen_addr = argv[2];
            std::cout << "acting as server, bound to " << udp_listen_addr << std::endl;
            // .1 is server
            multi_tun.tun_listen_addr = "10.42.42.1";
            multi_tun.init_server(udp_listen_addr);
        } else if (argc >= 4 && std::strcmp(argv[1], "client") == 0) {
            std::cout << "acting as client" << std::endl;
            // .2 is client
            multi_tun.tun_listen_addr = "10.42.42.2";
            multi_tun.init_client();
            for (int i = 2; i + 1 < argc; i += 2) {
                const std::string udp_listen_addr = argv[i];
                const std::string server_addr = argv[i + 1];
                std::cout << "bound to " << udp_listen_addr << ", connecting to " << server_addr
                          << std::endl;
                multi_tun.add_endpoint(udp_listen_addr, server_addr);
            }
        } else {
            std::cout << "usage:" << std::endl;
            std::cout << argv[0] << " server <bind addr>" << std::endl;
            std::cout << argv[0] << " client [<bind addr> <server addr>] ..." << std::endl;
            return -1;
        }
        multi_tun.run_loop();
    } catch (libsocket::socket_exception& e) { std::cerr << "error: " << e.mesg << std::endl; }
}