#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <util.h>
#include <termios.h>

// ---------------------------------------------------------------------------
// Word censoring
// ---------------------------------------------------------------------------

// Replace every occurrence of `word` (case-insensitive) with asterisks
// inside `data`. Only whole-chunk scanning; edges are NOT stitched across
// calls (by design – caller is responsible for passing complete chunks).
static void censor_word(std::string &data, const std::string &word)
{
    if (word.empty() || data.size() < word.size()) return;

    const size_t wlen = word.size();
    size_t pos = 0;

    while (pos + wlen <= data.size()) {
        // Case-insensitive compare
        bool match = true;
        for (size_t i = 0; i < wlen && match; ++i) {
            match = (std::tolower((unsigned char)data[pos + i]) ==
                     std::tolower((unsigned char)word[i]));
        }
        if (match) {
            std::fill(data.begin() + pos, data.begin() + pos + wlen, '*');
            pos += wlen;
        } else {
            ++pos;
        }
    }
}

// ---------------------------------------------------------------------------
// PATH search  (same semantics as execvp, but returns the full path)
// ---------------------------------------------------------------------------

static std::string find_in_path(const std::string &name)
{
    // If name already contains a slash, use as-is
    if (name.find('/') != std::string::npos)
        return name;

    const char *path_env = std::getenv("PATH");
    if (!path_env) path_env = "/usr/local/bin:/usr/bin:/bin";

    std::string path_str(path_env);
    size_t start = 0;
    while (true) {
        size_t colon = path_str.find(':', start);
        std::string dir = path_str.substr(start, colon == std::string::npos
                                                  ? std::string::npos
                                                  : colon - start);
        fprintf(stderr, "dir: %s\n", dir.c_str());
        if (dir.empty()) dir = ".";
        std::string full = dir + "/" + name;
        if (access(full.c_str(), X_OK) == 0)
            return full;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return {}; // not found
}

// ---------------------------------------------------------------------------
// Terminal helpers
// ---------------------------------------------------------------------------

static struct termios g_saved_termios;
static bool          g_raw_mode = false;

static void enter_raw_mode(int fd)
{
    if (!isatty(fd)) return;
    if (tcgetattr(fd, &g_saved_termios) < 0) return;

    struct termios raw = g_saved_termios;
    // raw / transparent mode: pass everything through unchanged
    raw.c_iflag &= ~(ICRNL | IXON | IXOFF | ISTRIP | BRKINT | INPCK);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cflag |=  CS8;
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(fd, TCSAFLUSH, &raw);
    g_raw_mode = true;
}

static void restore_termios()
{
    if (g_raw_mode)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
}

// Forward SIGWINCH (terminal resize) to the child PTY
static int g_pty_master = -1;
static pid_t g_child_pid = -1;

static void handle_sigwinch(int)
{
    if (g_pty_master < 0) return;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
        ioctl(g_pty_master, TIOCSWINSZ, &ws);
}

static void handle_sigterm(int sig)
{
    restore_termios();
    if (g_child_pid > 0) kill(g_child_pid, sig);
    _exit(128 + sig);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        return 1;
    }

    // ----- Locate binary ---------------------------------------------------
    std::string binary = find_in_path(argv[1]);
    if (binary.empty()) {
        fprintf(stderr, "%s: command not found: %s\n", argv[0], argv[1]);
        return 127;
    }

    // Build argv for exec
    std::vector<char *> child_argv;
    child_argv.push_back(argv[1]); // keep original name as argv[0]
    for (int i = 2; i < argc; ++i)
        child_argv.push_back(argv[i]);
    child_argv.push_back(nullptr);

    // ----- Decide: PTY or plain pipes --------------------------------------
    // Use a PTY only when our OWN stdout is a tty, so colours/ioctls work.
    bool use_pty = isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);

    int pty_master = -1;
    int pipe_stdin[2]  = {-1, -1}; // [read, write]
    int pipe_stdout[2] = {-1, -1};

    pid_t child_pid;

    if (use_pty) {
        // --- fork with a PTY -----------------------------------------------
        struct winsize ws = {};
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);

        child_pid = forkpty(&pty_master, nullptr, nullptr, &ws);
        if (child_pid < 0) {
            perror("forkpty");
            return 1;
        }
        if (child_pid == 0) {
            // Child: PTY is already set as stdin/stdout/stderr by forkpty
            execv(binary.c_str(), child_argv.data());
            perror(binary.c_str());
            _exit(127);
        }
        // Parent
        g_pty_master  = pty_master;
        g_child_pid   = child_pid;

        enter_raw_mode(STDIN_FILENO);

        signal(SIGWINCH, handle_sigwinch);
        signal(SIGTERM,  handle_sigterm);
        signal(SIGINT,   handle_sigterm);

        // Make master non-blocking
        fcntl(pty_master, F_SETFL, O_NONBLOCK);

    } else {
        // --- plain pipes ---------------------------------------------------
        if (pipe(pipe_stdin) < 0 || pipe(pipe_stdout) < 0) {
            perror("pipe");
            return 1;
        }

        child_pid = fork();
        if (child_pid < 0) {
            perror("fork");
            return 1;
        }
        if (child_pid == 0) {
            // Child: wire up pipes
            dup2(pipe_stdin[0],  STDIN_FILENO);
            dup2(pipe_stdout[1], STDOUT_FILENO);
            dup2(pipe_stdout[1], STDERR_FILENO);
            close(pipe_stdin[0]);  close(pipe_stdin[1]);
            close(pipe_stdout[0]); close(pipe_stdout[1]);
            execv(binary.c_str(), child_argv.data());
            perror(binary.c_str());
            _exit(127);
        }
        // Parent: close child-side ends
        close(pipe_stdin[0]);
        close(pipe_stdout[1]);
        g_child_pid = child_pid;
    }

    // -----------------------------------------------------------------------
    // Proxy loop
    // -----------------------------------------------------------------------
    const std::string BANNED = "penis";
    const size_t BUFSIZE = 4096;
    char buf[BUFSIZE];

    // fds we read from
    int read_from_child  = use_pty ? pty_master : pipe_stdout[0];
    int write_to_child   = use_pty ? pty_master : pipe_stdin[1];

    // Make our own stdin non-blocking
    int old_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags | O_NONBLOCK);
    fcntl(read_from_child, F_SETFL,
          fcntl(read_from_child, F_GETFL) | O_NONBLOCK);

    bool child_out_closed = false;
    bool stdin_closed     = false;

    while (true) {
        // Check if child exited and all output drained
        if (child_out_closed) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        if (!stdin_closed)     FD_SET(STDIN_FILENO, &rfds);
        if (!child_out_closed) FD_SET(read_from_child, &rfds);

        int maxfd = std::max(STDIN_FILENO, read_from_child) + 1;

        // Short timeout so we can detect child exit even when no data flows
        struct timeval tv = {0, 50000}; // 50 ms
        int sel = select(maxfd, &rfds, nullptr, nullptr, &tv);

        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // ---- stdin → child ------------------------------------------------
        if (!stdin_closed && FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, buf, BUFSIZE);
            if (n > 0) {
                // Write all to child
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(write_to_child, buf + written, n - written);
                    if (w < 0) { stdin_closed = true; break; }
                    written += w;
                }
            } else if (n == 0) {
                // EOF on our stdin: close write end to child
                if (!use_pty) close(pipe_stdin[1]);
                stdin_closed = true;
            }
            // EAGAIN / EWOULDBLOCK: just continue
        }

        // ---- child stdout → our stdout ------------------------------------
        if (!child_out_closed && FD_ISSET(read_from_child, &rfds)) {
            ssize_t n = read(read_from_child, buf, BUFSIZE);
            if (n > 0) {
                std::string chunk(buf, n);
                censor_word(chunk, BANNED);
                // Write full censored chunk
                const char *ptr = chunk.data();
                ssize_t left = (ssize_t)chunk.size();
                while (left > 0) {
                    ssize_t w = write(STDOUT_FILENO, ptr, left);
                    if (w < 0) break;
                    ptr  += w;
                    left -= w;
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                child_out_closed = true;
            }
        }

        // ---- detect child exit --------------------------------------------
        {
            int status = 0;
            pid_t r = waitpid(child_pid, &status, WNOHANG);
            if (r == child_pid) {
                // Drain any remaining output before exiting
                // One last non-blocking drain pass
                for (;;) {
                    ssize_t n = read(read_from_child, buf, BUFSIZE);
                    if (n <= 0) break;
                    std::string chunk(buf, n);
                    censor_word(chunk, BANNED);
                    const char *ptr = chunk.data();
                    ssize_t left = (ssize_t)chunk.size();
                    while (left > 0) {
                        ssize_t w = write(STDOUT_FILENO, ptr, left);
                        if (w < 0) break;
                        ptr  += w;
                        left -= w;
                    }
                }

                restore_termios();

                // Restore stdin flags
                fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags);

                if (WIFEXITED(status))   return WEXITSTATUS(status);
                if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
                return 1;
            }
        }
    }

    // If we broke out of the loop without waitpid succeeding, wait blocking
    restore_termios();
    fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags);

    int status = 0;
    waitpid(child_pid, &status, 0);
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
