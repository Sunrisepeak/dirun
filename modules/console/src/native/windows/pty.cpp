// The pty entries on a system without one in this build: the same faces,
// defined to say so at run time. A program that never opens a pty console is
// unaffected; one that does gets an actionable error, never a silent miss.
#include "../../pty_face.hpp"

namespace dirun::console::native {

namespace {
[[noreturn]] void not_built() {
    throw channel_error("console closed: the pty console driver is not built for this system yet");
}
}  // namespace

struct pty_state final {};

pty_state* pty_open(const std::vector<std::string>&) { not_built(); }
read_once pty_read_once(pty_state&, double, std::string&) { not_built(); }
void pty_write(pty_state&, const std::string&) { not_built(); }
int pty_master_fd(const pty_state&) { return -1; }
bool pty_child_alive(const pty_state&) { return false; }
void pty_close(pty_state*&) {}
void set_read_error_probe(int (*)()) {}

}  // namespace dirun::console::native
