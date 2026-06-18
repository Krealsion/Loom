#ifndef ZEN_ISOLATION_CHANNEL_HPP
#define ZEN_ISOLATION_CHANNEL_HPP

// A non-blocking, length-framed byte channel over a unix-domain socket fd — the
// parent side of a parent<->child Shard-host link. It is bounded and defensive:
// a frame larger than the cap, or an outbound backlog over the cap (a peer not
// draining), marks the channel failed — a misbehaving child is contained, never
// allowed to block, hang, or OOM the host. EOF (peer closed) is observable and
// signals child death.

#include <zen/isolation/protocol.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace zen::isolation {

struct Incoming {
    Op op = Op::Hello;
    std::string payload;
};

class Channel {
public:
    /// Takes ownership of `fd` and sets it non-blocking.
    explicit Channel(int fd);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    int fd() const noexcept { return fd_; }
    bool failed() const noexcept { return failed_; }
    bool eof() const noexcept { return eof_; }
    bool done() const noexcept { return failed_ || eof_; }

    /// Buffer a frame for sending; flush() writes it. An over-cap backlog fails
    /// the channel.
    void queue(Op op, std::string_view payload);

    /// Write as much of the outbound buffer as the socket accepts, without
    /// blocking (a full kernel buffer just leaves the rest queued for next time).
    void flush();

    /// Read available bytes without blocking and append every complete frame to
    /// `out`. Sets eof() on peer close and failed() on a protocol/IO error.
    void poll(std::vector<Incoming>& out);

private:
    int fd_;
    std::string outbox_;
    std::size_t out_pos_ = 0;
    std::string inbox_;
    bool failed_ = false;
    bool eof_ = false;
};

} // namespace zen::isolation

#endif // ZEN_ISOLATION_CHANNEL_HPP
