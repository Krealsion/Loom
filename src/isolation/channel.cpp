#include <zen/isolation/channel.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace zen::isolation {

namespace {
constexpr std::size_t kMaxBacklog = 64u * 1024u * 1024u; // a peer that won't drain is contained
} // namespace

Channel::Channel(int fd) : fd_(fd) {
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }
}

Channel::~Channel() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void Channel::queue(Op op, std::string_view payload) {
    if (failed_) {
        return;
    }
    if (payload.size() > kMaxFrameLen) {
        failed_ = true;
        return;
    }
    put_u32(outbox_, static_cast<std::uint32_t>(payload.size()));
    put_u8(outbox_, static_cast<std::uint8_t>(op));
    outbox_.append(payload);
    if (outbox_.size() - out_pos_ > kMaxBacklog) {
        failed_ = true;
    }
}

void Channel::flush() {
    if (failed_ || fd_ < 0) {
        return;
    }
    while (out_pos_ < outbox_.size()) {
        const ssize_t n =
            ::send(fd_, outbox_.data() + out_pos_, outbox_.size() - out_pos_, MSG_NOSIGNAL);
        if (n > 0) {
            out_pos_ += static_cast<std::size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // kernel buffer full; the rest stays queued for next flush
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            failed_ = true; // EPIPE / ECONNRESET / other: peer gone
            return;
        }
    }
    if (out_pos_ == outbox_.size()) {
        outbox_.clear();
        out_pos_ = 0;
    }
}

void Channel::poll(std::vector<Incoming>& out) {
    if (failed_ || fd_ < 0) {
        return;
    }
    char buf[8192];
    for (;;) {
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            inbox_.append(buf, static_cast<std::size_t>(n));
            if (inbox_.size() > kMaxBacklog) {
                failed_ = true;
                return;
            }
        } else if (n == 0) {
            eof_ = true;
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            failed_ = true;
            return;
        }
    }

    std::size_t pos = 0;
    while (inbox_.size() - pos >= 5) {
        Cursor header(std::string_view(inbox_).substr(pos, 5));
        std::uint32_t len = 0;
        std::uint8_t op = 0;
        (void)header.u32(len);
        (void)header.u8(op);
        if (len > kMaxFrameLen) {
            failed_ = true;
            break;
        }
        if (inbox_.size() - pos < static_cast<std::size_t>(5) + len) {
            break; // frame not fully arrived yet
        }
        out.push_back(Incoming{static_cast<Op>(op), inbox_.substr(pos + 5, len)});
        pos += static_cast<std::size_t>(5) + len;
    }
    if (pos > 0) {
        inbox_.erase(0, pos);
    }
}

} // namespace zen::isolation
