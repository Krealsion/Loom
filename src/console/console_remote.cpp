// SPDX-License-Identifier: MPL-2.0
// Copyright (c) 2026 Joshua DeMoss

// zen-console-remote: the REMOTE-operator console. It is exactly the in-process TUI (console_tui.cpp)
// with two differences, and only two: the engine lives behind a SOCKET (a RemoteConsole Console impl,
// not an in-process ConsoleEngine), and the synchronous read loop becomes an EVENT-DRIVEN
// single-threaded multiplexer over {terminal input, the socket}. The same ConsoleUi, the same shared
// renderer (tui_render), the same widget tree, the same key mapping — "remote is just another
// backend", proven by reuse. The bus stays entirely host-side and single-threaded; the multiplexer is
// the CLIENT's readiness-to-receive-from-many-sources, never bus concurrency.
//
// Usage: zen-console-remote [host=127.0.0.1] [port=7654]  (point it at a zen-bridge-host)

#include "tui_render.hpp"

#include <zen/bridge/remote_console.hpp>
#include <zen/console/ui.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#ifndef _WIN32
#include <unistd.h> // STDIN_FILENO
#endif

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port =
        argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : static_cast<std::uint16_t>(7654);

    std::string err;
    if (!loom::bridge_net_init(&err)) {
        std::fprintf(stderr, "zen-console-remote: net init failed: %s\n", err.c_str());
        return 1;
    }
    const loom::socket_t sock = loom::bridge_connect_tcp(host, port, &err);
    if (sock == loom::kInvalidSocket) {
        std::fprintf(stderr, "zen-console-remote: connect %s:%u failed: %s\n", host.c_str(), port,
                     err.c_str());
        return 1;
    }
    loom::RemoteConsole rc(sock);
    if (!rc.connected()) {
        std::fprintf(stderr, "zen-console-remote: handshake failed (is a zen-bridge-host listening "
                             "on %s:%u?)\n",
                     host.c_str(), port);
        return 1;
    }

    loom::ConsoleUi ui(rc); // the SAME controller as the in-process TUI, over the wire
    std::unique_ptr<loom::TerminalBackend> term = loom::make_terminal();

    int rows = 24, cols = 80;
    if (!term->size(rows, cols)) {
        rows = 24;
        cols = 80;
    }
    loom::tui_draw(ui.tree(), rows, cols, *term);

    const int kTickMs = 30; // bounds socket latency on the Windows deadline-loop; idle wait on POSIX
    bool quit = false;
    while (!quit) {
#ifndef _WIN32
        // The multiplexer: block until EITHER stdin or the socket is ready, or the tick elapses. A
        // socket delivers when the FAR side decides; the tap pushes unbidden; disconnect is a
        // readable-then-EOF socket — all of which this surfaces and a synchronous read could not.
        (void)loom::bridge_wait_readable({static_cast<loom::socket_t>(STDIN_FILENO), rc.socket()},
                                         kTickMs);
#endif
        // Drain the socket (discovery replies, the tap stream, buffered replies); repaint if changed.
        rc.pump();
        if (rc.disconnected()) {
            break; // the host went away — handled as an event, not a hang
        }
        if (rc.take_dirty().any()) {
            if (!term->size(rows, cols)) {
                rows = 24;
                cols = 80;
            }
            loom::tui_draw(ui.tree(), rows, cols, *term);
        }

        // Read input: on POSIX we already selected, so poll non-blocking; on Windows the terminal's
        // WaitForSingleObject deadline-loop bounds the wait (and the socket is drained each turn above).
#ifndef _WIN32
        const int c = term->read_byte_timeout(0);
#else
        const int c = term->read_byte_timeout(kTickMs);
#endif
        if (c >= 0) {
            loom::InputEvent ev;
            if (!loom::tui_map_key(c, *term, ev)) {
                quit = true; // Ctrl-X
                break;
            }
            ui.dispatch(ev); // Submit composes via the shared ladder and ships a Send over the wire
            (void)rc.take_dirty();
            if (!term->size(rows, cols)) {
                rows = 24;
                cols = 80;
            }
            loom::tui_draw(ui.tree(), rows, cols, *term);
        }
    }

    term->write("\x1b[H\x1b[2J");
    term->flush();
    if (rc.disconnected() && !quit) {
        std::fprintf(stderr, "zen-console-remote: the host closed the connection.\n");
    }
    return 0;
} // term's dtor restores cooked mode here
