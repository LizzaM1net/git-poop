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
#ifdef __APPLE__
    #include <util.h>
#else
    #include <pty.h>
#endif
#include <termios.h>

// ---------------------------------------------------------------------------
// Word censoring
// ---------------------------------------------------------------------------

static void censor_inplace(char *data, size_t len, const char *word)
{
    const size_t wlen = strlen(word);
    if (wlen == 0 || len < wlen) return;
    size_t pos = 0;
    while (pos + wlen <= len) {
        if (strncasecmp(data + pos, word, wlen) == 0) {
            memset(data + pos, '*', wlen);
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

const char *BANNED = "penis";
const size_t BUFSIZE = 4096;
char buf[BUFSIZE];

static bool proxy_child_output(int read_fd, int write_fd)
{
    ssize_t n = read(read_fd, buf, BUFSIZE);
    if (n > 0) {
        censor_inplace(buf, n, BANNED);

        const char *ptr = buf;
        while (n > 0) {
            ssize_t w = write(write_fd, ptr, n);
            if (w < 0) break;
            ptr  += w;
            n -= w;
        }
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        return true;
    }

    return false;
}

static bool proxy_child_output_drain(int read_fd, int write_fd)
{
    ssize_t n = read(read_fd, buf, BUFSIZE);
    if (n > 0) {
        censor_inplace(buf, n, BANNED);
        const char *ptr = buf;
        while (n > 0) {
            ssize_t w = write(write_fd, ptr, n);
            if (w < 0) return true;
            ptr += w; n -= w;
        }
        return false;
    }
    // n == 0 (EOF на pipe) или EIO (PTY master после закрытия slave)
    if (n < 0 && errno == EAGAIN) return false; // не должно быть, но на всякий случай
    return true; // EOF или EIO — дренаж завершён
}

int main(int argc, char *argv[])
{
    if (argc < 2)
        return 0;

    std::string binary = find_in_path(argv[1]);
    if (binary.empty()) {
        fprintf(stderr, "%s: command not found: %s\n", argv[0], argv[1]);
        return 127;
    }

    std::vector<char *> child_argv(argv + 1, argv + argc);
    child_argv.push_back(nullptr);

    // ----- Decide: PTY or plain pipes --------------------------------------
    // Use a PTY only when our OWN stdout is a tty, so colours/ioctls work.
    bool use_pty = isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);

    int pty_master = -1;
    int pipe_stdin[2]  = {-1, -1}; // [read, write]
    int pipe_stdout[2] = {-1, -1};
    int pipe_stderr[2] = {-1, -1};

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
        if (pipe(pipe_stdin) < 0 || pipe(pipe_stdout) < 0 || pipe(pipe_stderr) < 0) {
            perror("pipe");
            return 1;
        }

        child_pid = fork();
        if (child_pid < 0) {
            perror("fork");
            return 1;
        }
        if (child_pid == 0) {
            dup2(pipe_stdin[0],  STDIN_FILENO);
            dup2(pipe_stdout[1], STDOUT_FILENO);
            dup2(pipe_stderr[1], STDERR_FILENO);
            close(pipe_stdin[0]);  close(pipe_stdin[1]);
            close(pipe_stdout[0]); close(pipe_stdout[1]);
            close(pipe_stderr[0]); close(pipe_stderr[1]);
            execv(binary.c_str(), child_argv.data());
            perror(binary.c_str());
            _exit(127);
        }
        // Parent: close child-side ends
        close(pipe_stdin[0]);
        close(pipe_stdout[1]);
        close(pipe_stderr[1]);
        g_child_pid = child_pid;
    }

    // fds we read from
    int read_from_child  = use_pty ? pty_master : pipe_stdout[0];
    int error_from_child = pipe_stderr[0];
    int write_to_child   = use_pty ? pty_master : pipe_stdin[1];

    // Make our own stdin non-blocking
    int old_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags | O_NONBLOCK);
    fcntl(read_from_child, F_SETFL,
          fcntl(read_from_child, F_GETFL) | O_NONBLOCK);
    if (!use_pty) {
        fcntl(error_from_child, F_SETFL,
              fcntl(error_from_child, F_GETFL) | O_NONBLOCK);
    }

    bool child_out_closed = false;
    bool child_err_closed = false;
    bool stdin_closed     = false;

    while (true) {
        // Check if child exited and all output drained
        if (child_out_closed && child_err_closed) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        if (!stdin_closed)     FD_SET(STDIN_FILENO, &rfds);
        if (!child_out_closed) FD_SET(read_from_child, &rfds);
        if (!child_err_closed && !use_pty) FD_SET(error_from_child, &rfds);

        int maxfd = std::max({STDIN_FILENO, read_from_child, error_from_child}) + 1;

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
            usleep(1000000);
            child_out_closed = proxy_child_output(read_from_child, STDOUT_FILENO);
        }

        // ---- child stderr → our stderr ------------------------------------
        if (!child_err_closed && FD_ISSET(error_from_child, &rfds)) {
            usleep(1000000);
            child_err_closed = proxy_child_output(error_from_child, STDERR_FILENO);
        }

        // ---- detect child exit --------------------------------------------
        {
            int status = 0;
            pid_t r = waitpid(child_pid, &status, WNOHANG);
            if (r == child_pid) {
                fcntl(read_from_child, F_SETFL,
                          fcntl(read_from_child, F_GETFL) & ~O_NONBLOCK);
                if (!use_pty) {
                    fcntl(error_from_child, F_SETFL,
                            fcntl(error_from_child, F_GETFL) & ~O_NONBLOCK);
                }

                // fcntl(read_from_child, F_SETFL, fcntl(read_from_child, F_GETFL) & ~O_NONBLOCK);
                while (!child_out_closed)
                    child_out_closed = proxy_child_output_drain(read_from_child, STDOUT_FILENO);
                    // child_out_closed = proxy_child_output(read_from_child, STDOUT_FILENO);

                // fcntl(error_from_child, F_SETFL, fcntl(error_from_child, F_GETFL) & ~O_NONBLOCK);
                while (!child_err_closed)
                    child_err_closed = proxy_child_output_drain(error_from_child, STDERR_FILENO);
                    // child_err_closed = proxy_child_output(error_from_child, STDERR_FILENO);

                printf("process exited\r\n");
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
