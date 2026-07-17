#include "tcp_connection.h"
#include "log.h"

#ifdef WINDOWS
#ifndef MINGW
#pragma comment(lib, "Ws2_32.lib") // Without this, Ws2_32.lib is not included in static library.
#endif
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <cassert>
#include <utility>

#ifndef WINDOWS
#define GET_ERROR(_x) strerror(_x)
#else
#define GET_ERROR(_x) WSAGetLastError()
#endif

namespace mavsdk {

namespace {
constexpr int CONNECT_TIMEOUT_MS = 3000;
}

/* change to remote_ip and remote_port */
TcpConnection::TcpConnection(
    Connection::ReceiverCallback receiver_callback,
    std::string remote_ip,
    int remote_port,
    ForwardingOption forwarding_option) :
    Connection(std::move(receiver_callback), forwarding_option),
    _remote_ip(std::move(remote_ip)),
    _remote_port_number(remote_port),
    _should_exit(false)
{}

TcpConnection::~TcpConnection()
{
    // If no one explicitly called stop before, we should at least do it.
    stop();
}

ConnectionResult TcpConnection::start()
{
    if (!start_mavlink_receiver()) {
        return ConnectionResult::ConnectionsExhausted;
    }

    ConnectionResult ret = setup_port();
    if (ret != ConnectionResult::Success) {
        return ret;
    }

    start_recv_thread();

    return ConnectionResult::Success;
}

ConnectionResult TcpConnection::setup_port()
{
#ifdef WINDOWS
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LogErr() << "Error: Winsock failed, error: %d", WSAGetLastError();
        _is_ok = false;
        return ConnectionResult::SocketError;
    }
#endif

    _socket_fd.reset(socket(AF_INET, SOCK_STREAM, 0));

    if (_socket_fd.empty()) {
        LogErr() << "socket error" << GET_ERROR(errno);
        _is_ok = false;
        return ConnectionResult::SocketError;
    }

    struct sockaddr_in remote_addr {};
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(_remote_port_number);

    struct hostent* hp;
    hp = gethostbyname(_remote_ip.c_str());
    if (hp == nullptr) {
        LogErr() << "Could not get host by name";
        _is_ok = false;
        return ConnectionResult::SocketConnectionError;
    }

    memcpy(&remote_addr.sin_addr, hp->h_addr, hp->h_length);

#ifdef WINDOWS
    u_long non_blocking = 1;
    if (ioctlsocket(_socket_fd.get(), FIONBIO, &non_blocking) != 0) {
        LogErr() << "ioctlsocket set non-blocking error: " << WSAGetLastError();
        _is_ok = false;
        return ConnectionResult::SocketConnectionError;
    }

    auto restore_blocking = [this]() {
        u_long blocking = 0;
        if (ioctlsocket(_socket_fd.get(), FIONBIO, &blocking) != 0) {
            LogErr() << "ioctlsocket restore blocking error: " << WSAGetLastError();
            return false;
        }
        return true;
    };

    if (connect(
            _socket_fd.get(),
            reinterpret_cast<sockaddr*>(&remote_addr),
            sizeof(struct sockaddr_in)) < 0) {
        const auto connect_error = WSAGetLastError();
        if (connect_error != WSAEWOULDBLOCK && connect_error != WSAEINPROGRESS) {
            LogErr() << "connect error: " << connect_error;
            restore_blocking();
            _is_ok = false;
            return ConnectionResult::SocketConnectionError;
        }

        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(_socket_fd.get(), &write_fds);

        timeval timeout {};
        timeout.tv_sec = CONNECT_TIMEOUT_MS / 1000;
        timeout.tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000;

        const auto select_result = select(0, nullptr, &write_fds, nullptr, &timeout);
        if (select_result == 0) {
            LogErr() << "connect timeout";
            restore_blocking();
            _is_ok = false;
            return ConnectionResult::Timeout;
        }
        if (select_result < 0) {
            LogErr() << "connect select error: " << WSAGetLastError();
            restore_blocking();
            _is_ok = false;
            return ConnectionResult::SocketConnectionError;
        }

        int socket_error = 0;
        int socket_error_len = sizeof(socket_error);
        if (getsockopt(
                _socket_fd.get(),
                SOL_SOCKET,
                SO_ERROR,
                reinterpret_cast<char*>(&socket_error),
                &socket_error_len) < 0) {
            LogErr() << "connect getsockopt error: " << WSAGetLastError();
            restore_blocking();
            _is_ok = false;
            return ConnectionResult::SocketConnectionError;
        }
        if (socket_error != 0) {
            LogErr() << "connect error: " << socket_error;
            restore_blocking();
            _is_ok = false;
            return ConnectionResult::SocketConnectionError;
        }
    }

    if (!restore_blocking()) {
        _is_ok = false;
        return ConnectionResult::SocketConnectionError;
    }
#else
    const auto flags = fcntl(_socket_fd.get(), F_GETFL, 0);
    if (flags == -1) {
        LogErr() << "fcntl get flags error: " << GET_ERROR(errno);
        _is_ok = false;
        return ConnectionResult::SocketConnectionError;
    }

    if (fcntl(_socket_fd.get(), F_SETFL, flags | O_NONBLOCK) == -1) {
        LogErr() << "fcntl set non-blocking error: " << GET_ERROR(errno);
        _is_ok = false;
        return ConnectionResult::SocketConnectionError;
    }

    auto restore_flags = [this, flags]() {
        if (fcntl(_socket_fd.get(), F_SETFL, flags) == -1) {
            LogErr() << "fcntl restore flags error: " << GET_ERROR(errno);
            return false;
        }
        return true;
    };

    if (connect(
            _socket_fd.get(),
            reinterpret_cast<sockaddr*>(&remote_addr),
            sizeof(struct sockaddr_in)) < 0) {
        if (errno != EINPROGRESS) {
            LogErr() << "connect error: " << GET_ERROR(errno);
            restore_flags();
            _is_ok = false;
            return ConnectionResult::SocketConnectionError;
        }

        pollfd socket_poll {};
        socket_poll.fd = _socket_fd.get();
        socket_poll.events = POLLOUT;

        const auto poll_result = poll(&socket_poll, 1, CONNECT_TIMEOUT_MS);
        if (poll_result == 0) {
            LogErr() << "connect timeout";
            restore_flags();
            _is_ok = false;
            return ConnectionResult::Timeout;
        }
        if (poll_result < 0) {
            LogErr() << "connect poll error: " << GET_ERROR(errno);
            restore_flags();
            _is_ok = false;
            return ConnectionResult::SocketConnectionError;
        }

        int socket_error = 0;
        socklen_t socket_error_len = sizeof(socket_error);
        if (getsockopt(
                _socket_fd.get(), SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) < 0) {
            LogErr() << "connect getsockopt error: " << GET_ERROR(errno);
            restore_flags();
            _is_ok = false;
            return ConnectionResult::SocketConnectionError;
        }
        if (socket_error != 0) {
            LogErr() << "connect error: " << GET_ERROR(socket_error);
            restore_flags();
            _is_ok = false;
            return ConnectionResult::SocketConnectionError;
        }
    }

    if (!restore_flags()) {
        _is_ok = false;
        return ConnectionResult::SocketConnectionError;
    }
#endif

    _is_ok = true;
    return ConnectionResult::Success;
}

void TcpConnection::start_recv_thread()
{
    _recv_thread = std::make_unique<std::thread>(&TcpConnection::receive, this);
}

ConnectionResult TcpConnection::stop()
{
    _should_exit = true;

    if (!_socket_fd.empty()) {
#ifdef WINDOWS
        shutdown(_socket_fd.get(), SD_BOTH);
#else
        shutdown(_socket_fd.get(), SHUT_RDWR);
#endif
    }
    _socket_fd.close();

    if (_recv_thread) {
        _recv_thread->join();
        _recv_thread.reset();
    }

    // We need to stop this after stopping the receive thread, otherwise
    // it can happen that we interfere with the parsing of a message.
    stop_mavlink_receiver();

    return ConnectionResult::Success;
}

bool TcpConnection::send_message(const mavlink_message_t& message)
{
    if (!_is_ok) {
        return false;
    }

    if (_remote_ip.empty()) {
        LogErr() << "Remote IP unknown";
        return false;
    }

    if (_remote_port_number == 0) {
        LogErr() << "Remote port unknown";
        return false;
    }

    struct sockaddr_in dest_addr {};
    dest_addr.sin_family = AF_INET;

    inet_pton(AF_INET, _remote_ip.c_str(), &dest_addr.sin_addr.s_addr);

    dest_addr.sin_port = htons(_remote_port_number);

    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    uint16_t buffer_len = mavlink_msg_to_send_buffer(buffer, &message);

    // TODO: remove this assert again
    assert(buffer_len <= MAVLINK_MAX_PACKET_LEN);

#if !defined(MSG_NOSIGNAL)
    auto flags = 0;
#else
    auto flags = MSG_NOSIGNAL;
#endif

    const auto send_len = sendto(
        _socket_fd.get(),
        reinterpret_cast<char*>(buffer),
        buffer_len,
        flags,
        reinterpret_cast<const sockaddr*>(&dest_addr),
        sizeof(dest_addr));

    if (send_len != buffer_len) {
        LogErr() << "sendto failure: " << GET_ERROR(errno);
        _is_ok = false;
        return false;
    }
    return true;
}

void TcpConnection::receive()
{
    // Enough for MTU 1500 bytes.
    char buffer[2048];

    while (!_should_exit) {
        if (!_is_ok) {
            LogErr() << "TCP receive error, trying to reconnect...";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            setup_port();
        }

        const auto recv_len = recv(_socket_fd.get(), buffer, sizeof(buffer), 0);

        if (recv_len == 0) {
            // This can happen when shutdown is called on the socket,
            // therefore we check _should_exit again.
            _is_ok = false;
            continue;
        }

        if (recv_len < 0) {
            // This happens on destruction when close(_socket_fd.get()) is called,
            // therefore be quiet.
            // LogErr() << "recvfrom error: " << GET_ERROR(errno);
            // Something went wrong, we should try to re-connect in next iteration.
            _is_ok = false;
            continue;
        }

        _mavlink_receiver->set_new_datagram(buffer, static_cast<int>(recv_len));

        // Parse all mavlink messages in one data packet. Once exhausted, we'll exit while.
        while (_mavlink_receiver->parse_message()) {
            receive_message(_mavlink_receiver->get_last_message(), this);
        }
    }
}

} // namespace mavsdk
