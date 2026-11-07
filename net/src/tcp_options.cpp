#include "skycue/net/tcp_options.hpp"

#include "skycue/net/log.hpp"

#include <boost/asio/socket_base.hpp>

#ifdef __linux__
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <string>

namespace skycue::net {

namespace {

void note(const char* what, const boost::system::error_code& ec)
{
    if (ec)
        log(LogLevel::debug, std::string("tcp option ") + what + ": " + ec.message());
}

} // namespace

void applyTcpOptions(boost::asio::ip::tcp::socket& s, const TcpOptions& o)
{
    boost::system::error_code ec;
    s.set_option(boost::asio::ip::tcp::no_delay(o.nodelay), ec);
    note("TCP_NODELAY", ec);
    s.set_option(boost::asio::socket_base::keep_alive(o.keepalive), ec);
    note("SO_KEEPALIVE", ec);
#ifdef __linux__
    if (o.keepalive) {
        const int fd = s.native_handle();
        const int idle = 30, interval = 10, count = 3;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle) != 0)
            log(LogLevel::debug, "tcp option TCP_KEEPIDLE failed");
        if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval) != 0)
            log(LogLevel::debug, "tcp option TCP_KEEPINTVL failed");
        if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count) != 0)
            log(LogLevel::debug, "tcp option TCP_KEEPCNT failed");
    }
#endif
}

} // namespace skycue::net
