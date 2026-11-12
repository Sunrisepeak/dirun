// The seam between the console driver and the pty implementation of a system.
// One implementation per target, chosen by the manifest; a misuse is a link
// error, never a branch at run time.
#pragma once

#include <exception>
#include <string>
#include <vector>

namespace dirun::console::native {

// The driver's two failure faces, mirroring what the specification froze:
// a channel that broke, and a program that could not be started at all.
class channel_error : public std::exception {
public:
    explicit channel_error(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

class spawn_error : public std::exception {
public:
    explicit spawn_error(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

// What one read attempt produced.
enum class read_once {
    bytes,    // new bytes arrived
    waiting,  // the slice elapsed, nothing yet
    ended,    // the child is gone: the channel is at its end
};

struct pty_state;

pty_state* pty_open(const std::vector<std::string>& argv);
read_once pty_read_once(pty_state& state, double timeout_seconds, std::string& into);
void pty_write(pty_state& state, const std::string& bytes);
int pty_master_fd(const pty_state& state);
bool pty_child_alive(const pty_state& state);
// Ends the child's whole process group, waits, closes the master, frees; sets
// the handle to null. Idempotent.
void pty_close(pty_state*& handle);

// A test seam: the errno the next read fails with (0 clears it).
void set_read_error_probe(int (*probe)());

}  // namespace dirun::console::native
