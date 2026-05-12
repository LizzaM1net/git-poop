#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  define strncasecmp _strnicmp

// ConPTY types/constants defined manually for older SDK compatibility.
// CreatePseudoConsole / ClosePseudoConsole are loaded at runtime so the
// binary still runs (pipe fallback) on Windows < 10 1809.
#  ifndef HPCON
     typedef VOID *HPCON;
#  endif
#  ifndef PSEUDOCONSOLE_INHERIT_CURSOR
#    define PSEUDOCONSOLE_INHERIT_CURSOR ((DWORD)0x00000002)
#  endif
#  ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#    define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ((DWORD_PTR)0x00020016)
#  endif

#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <sys/select.h>
#  include <sys/ioctl.h>
#  ifdef __APPLE__
#    include <util.h>
#  else
#    include <pty.h>
#  endif
#  include <termios.h>
#endif

// ---------------------------------------------------------------------------
// Word censoring (shared)
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

const char   *BANNED  = "penis";
const size_t  BUFSIZE = 4096;

// ===========================================================================
// Windows implementation
// ===========================================================================
#ifdef _WIN32

typedef HRESULT (WINAPI *PfnCreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef void    (WINAPI *PfnClosePseudoConsole)(HPCON);

static PfnCreatePseudoConsole pfn_create_pty = NULL;
static PfnClosePseudoConsole  pfn_close_pty  = NULL;

// Manual-reset event set by the main thread once the child exits.
// Unblocks stdin_forward_thread so it stops trying to write to a dead child.
static HANDLE g_stop_event = NULL;

struct PipeThreadArgs {
    HANDLE read_from;
    HANDLE write_to;
    bool   censor_output;
};

// Drains a pipe until the write end closes (ERROR_BROKEN_PIPE), then exits.
// With plain pipes: child is the sole writer, so this returns as soon as the
// child exits.  With ConPTY: conhost holds the write end, so we must call
// ClosePseudoConsole() first to release conhost and unblock this thread.
static DWORD WINAPI pipe_copy_thread(LPVOID param)
{
    auto  *a = static_cast<PipeThreadArgs *>(param);
    char   buf[BUFSIZE];
    DWORD  n;
    while (ReadFile(a->read_from, buf, (DWORD)BUFSIZE, &n, NULL) && n > 0) {
        if (a->censor_output)
            censor_inplace(buf, n, BANNED);
        DWORD remaining = n;
        const char *ptr = buf;
        while (remaining > 0) {
            DWORD written;
            if (!WriteFile(a->write_to, ptr, remaining, &written, NULL))
                remaining = 0;
            else {
                ptr       += written;
                remaining -= written;
            }
        }
    }
    return 0;
}

// Forwards our stdin to the child's input handle.  Uses WaitForMultipleObjects
// so it wakes immediately on g_stop_event rather than blocking in ReadFile
// after the child has gone.
static DWORD WINAPI stdin_forward_thread(LPVOID param)
{
    HANDLE write_to_child = static_cast<HANDLE>(param);
    HANDLE stdin_h        = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE wait_h[2]      = { stdin_h, g_stop_event };
    char   buf[BUFSIZE];
    DWORD  n;

    while (true) {
        DWORD res = WaitForMultipleObjects(2, wait_h, FALSE, INFINITE);
        if (res != WAIT_OBJECT_0) break;   // stop event or error
        if (!ReadFile(stdin_h, buf, (DWORD)BUFSIZE, &n, NULL) || n == 0) break;
        DWORD written;
        if (!WriteFile(write_to_child, buf, n, &written, NULL)) break;
    }
    return 0;
}

static std::string build_cmdline(const std::vector<char *> &args)
{
    std::string result;
    for (const char *arg : args) {
        if (!arg) break;
        if (!result.empty()) result += ' ';
        bool needs_quotes = strpbrk(arg, " \t\"") != nullptr;
        if (needs_quotes) result += '"';
        for (const char *c = arg; *c; ++c) {
            if (*c == '"') result += '\\';
            result += *c;
        }
        if (needs_quotes) result += '"';
    }
    return result;
}

// PTY path: ConPTY gives the child a real TTY so it enables colour output,
// invokes a pager, etc. — mirroring the Unix forkpty path.
//
// PSEUDOCONSOLE_INHERIT_CURSOR: without this, ConPTY emits a cursor-home
// sequence on init that makes PowerShell scroll all previous output away.
// With it, ConPTY inherits the current cursor position and outputs in-place.
//
// ClosePseudoConsole after child exit: without this, conhost.exe keeps the
// write end of pty_out alive, so the output thread blocks in ReadFile forever.
// Closing the pseudo console terminates conhost, releasing the handle, and
// the thread gets ERROR_BROKEN_PIPE and exits.
static int run_windows_pty(const std::string &cmdline,
                            const std::vector<char *> &argv_vec)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);

    DWORD out_mode = 0, in_mode = 0;
    GetConsoleMode(hOut, &out_mode);
    GetConsoleMode(hIn,  &in_mode);

    // Enable VT processing so coloured child output renders correctly.
    SetConsoleMode(hOut, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    // Raw stdin: no echo/line-buffering; child's TTY line discipline takes over.
    SetConsoleMode(hIn,
        (in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT))
        | ENABLE_VIRTUAL_TERMINAL_INPUT);

    HANDLE pty_in_r, pty_in_w;
    HANDLE pty_out_r, pty_out_w;
    CreatePipe(&pty_in_r,  &pty_in_w,  NULL, 0);
    CreatePipe(&pty_out_r, &pty_out_w, NULL, 0);

    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    GetConsoleScreenBufferInfo(hOut, &csbi);
    COORD sz = {
        (SHORT)std::max(1, (int)(csbi.srWindow.Right  - csbi.srWindow.Left + 1)),
        (SHORT)std::max(1, (int)(csbi.srWindow.Bottom - csbi.srWindow.Top  + 1)),
    };

    HPCON hpc = NULL;
    if (FAILED(pfn_create_pty(sz, pty_in_r, pty_out_w,
                               PSEUDOCONSOLE_INHERIT_CURSOR, &hpc))) {
        CloseHandle(pty_in_r);  CloseHandle(pty_in_w);
        CloseHandle(pty_out_r); CloseHandle(pty_out_w);
        SetConsoleMode(hOut, out_mode);
        SetConsoleMode(hIn,  in_mode);
        fputs("git-poop: CreatePseudoConsole failed\n", stderr);
        return 1;
    }
    // ConPTY now owns these ends.
    CloseHandle(pty_in_r);
    CloseHandle(pty_out_w);

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    auto attr = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_size);
    InitializeProcThreadAttributeList(attr, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              hpc, sizeof(HPCON), NULL, NULL);

    STARTUPINFOEXA si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attr;

    // Writable copy: CreateProcessA may modify lpCommandLine in place.
    std::string cmd = cmdline;
    PROCESS_INFORMATION pi = {};
    // No STARTF_USESTDHANDLES: ConPTY wires the child's stdio automatically.
    if (!CreateProcessA(NULL, cmd.data(), NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                        &si.StartupInfo, &pi)) {
        fprintf(stderr, "git-poop: cannot start '%s' (error %lu)\n",
                argv_vec.empty() ? "?" : argv_vec[0], GetLastError());
        pfn_close_pty(hpc);
        DeleteProcThreadAttributeList(attr);
        HeapFree(GetProcessHeap(), 0, attr);
        CloseHandle(pty_in_w);
        CloseHandle(pty_out_r);
        SetConsoleMode(hOut, out_mode);
        SetConsoleMode(hIn,  in_mode);
        return 1;
    }
    CloseHandle(pi.hThread);

    g_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    PipeThreadArgs out_args = { pty_out_r, hOut, true };
    HANDLE out_thr = CreateThread(NULL, 0, pipe_copy_thread,     &out_args, 0, NULL);
    HANDLE in_thr  = CreateThread(NULL, 0, stdin_forward_thread, pty_in_w,  0, NULL);

    WaitForSingleObject(pi.hProcess, INFINITE);

    pfn_close_pty(hpc);      // terminates conhost → output thread gets BROKEN_PIPE
    SetEvent(g_stop_event);  // unblocks stdin thread

    HANDLE io_threads[2] = { out_thr, in_thr };
    WaitForMultipleObjects(2, io_threads, TRUE, 5000);
    CloseHandle(pty_in_w);   // safe: stdin thread confirmed done

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    SetConsoleMode(hOut, out_mode);
    SetConsoleMode(hIn,  in_mode);

    CloseHandle(pi.hProcess);
    CloseHandle(pty_out_r);
    CloseHandle(out_thr);
    CloseHandle(in_thr);
    CloseHandle(g_stop_event);
    DeleteProcThreadAttributeList(attr);
    HeapFree(GetProcessHeap(), 0, attr);

    return (int)exit_code;
}

// Pipe path: used when stdout is not a console (redirected / scripted) or as
// a fallback on Windows < 10 1809 where ConPTY is unavailable.
// Child sees pipes rather than a TTY, so isatty()-gated features (colour,
// pager) are disabled in the child — same trade-off as Unix pipe mode.
static int run_windows_pipes(const std::string &cmdline,
                              const std::vector<char *> &argv_vec)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    HANDLE child_stdin_r,  child_stdin_w;
    HANDLE child_stdout_r, child_stdout_w;
    HANDLE child_stderr_r, child_stderr_w;

    if (!CreatePipe(&child_stdin_r,  &child_stdin_w,  &sa, 0) ||
        !CreatePipe(&child_stdout_r, &child_stdout_w, &sa, 0) ||
        !CreatePipe(&child_stderr_r, &child_stderr_w, &sa, 0)) {
        fputs("git-poop: CreatePipe failed\n", stderr);
        return 1;
    }

    // Prevent parent-side ends from being inherited by the child.
    SetHandleInformation(child_stdin_w,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdout_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stderr_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb           = sizeof(si);
    si.dwFlags      = STARTF_USESTDHANDLES;
    si.hStdInput    = child_stdin_r;
    si.hStdOutput   = child_stdout_w;
    si.hStdError    = child_stderr_w;

    std::string cmd = cmdline;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(NULL, cmd.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "git-poop: cannot start '%s' (error %lu)\n",
                argv_vec.empty() ? "?" : argv_vec[0], GetLastError());
        return 1;
    }

    CloseHandle(child_stdin_r);
    CloseHandle(child_stdout_w);
    CloseHandle(child_stderr_w);
    CloseHandle(pi.hThread);

    g_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    PipeThreadArgs out_args = { child_stdout_r, GetStdHandle(STD_OUTPUT_HANDLE), true };
    PipeThreadArgs err_args = { child_stderr_r, GetStdHandle(STD_ERROR_HANDLE),  true };

    HANDLE out_thr = CreateThread(NULL, 0, pipe_copy_thread,     &out_args,     0, NULL);
    HANDLE err_thr = CreateThread(NULL, 0, pipe_copy_thread,     &err_args,     0, NULL);
    HANDLE in_thr  = CreateThread(NULL, 0, stdin_forward_thread, child_stdin_w, 0, NULL);

    WaitForSingleObject(pi.hProcess, INFINITE);
    SetEvent(g_stop_event);

    HANDLE io_threads[3] = { out_thr, err_thr, in_thr };
    WaitForMultipleObjects(3, io_threads, TRUE, 5000);
    CloseHandle(child_stdin_w);  // safe: stdin thread confirmed done

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(child_stdout_r);
    CloseHandle(child_stderr_r);
    CloseHandle(out_thr);
    CloseHandle(err_thr);
    CloseHandle(in_thr);
    CloseHandle(g_stop_event);

    return (int)exit_code;
}

static int run_windows(const std::vector<char *> &argv_vec)
{
    // Load ConPTY at runtime — absent on Windows < 10 1809, fall back to pipes.
    pfn_create_pty = (PfnCreatePseudoConsole)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "CreatePseudoConsole");
    pfn_close_pty = (PfnClosePseudoConsole)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "ClosePseudoConsole");

    std::string cmdline = build_cmdline(argv_vec);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);

    // Use PTY only when both stdio are real console handles, mirroring
    // the Unix isatty() check that gates forkpty vs plain pipes.
    bool use_pty = pfn_create_pty && pfn_close_pty &&
                   GetFileType(hOut) == FILE_TYPE_CHAR &&
                   GetFileType(hIn)  == FILE_TYPE_CHAR;

    return use_pty ? run_windows_pty(cmdline, argv_vec)
                   : run_windows_pipes(cmdline, argv_vec);
}

// ===========================================================================
// Unix implementation
// ===========================================================================
#else

static struct termios g_saved_termios;
static bool           g_raw_mode   = false;
static int            g_pty_master = -1;
static pid_t          g_child_pid  = -1;

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

static bool proxy_child_output(int read_fd, int write_fd)
{
    char buf[BUFSIZE];
    ssize_t n = read(read_fd, buf, BUFSIZE);
    if (n > 0) {
        censor_inplace(buf, n, BANNED);

        const char *ptr = buf;
        while (n > 0) {
            ssize_t w = write(write_fd, ptr, n);
            if (w < 0) break;
            ptr += w;
            n   -= w;
        }
    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        return true;
    }

    return false;
}

static int run_unix(char *command, const std::vector<char *> &child_argv)
{
    // Use a PTY only when our OWN stdout is a tty, so colours/ioctls work.
    bool use_pty = isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);

    int pty_master     = -1;
    int pipe_stdin[2]  = {-1, -1};
    int pipe_stdout[2] = {-1, -1};
    int pipe_stderr[2] = {-1, -1};

    pid_t child_pid;

    if (use_pty) {
        struct winsize ws = {};
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);

        child_pid = forkpty(&pty_master, nullptr, nullptr, &ws);
        if (child_pid < 0) {
            perror("forkpty");
            return 1;
        }
        if (child_pid == 0) {
            // Child: PTY is already set as stdin/stdout/stderr by forkpty
            execvp(command, child_argv.data());
            perror(command);
            _exit(127);
        }
        // Parent
        g_pty_master = pty_master;
        g_child_pid  = child_pid;

        enter_raw_mode(STDIN_FILENO);
        signal(SIGWINCH, handle_sigwinch);
        signal(SIGTERM,  handle_sigterm);
        signal(SIGINT,   handle_sigterm);

        fcntl(pty_master, F_SETFL, O_NONBLOCK);

    } else {
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
            execvp(command, child_argv.data());
            perror(command);
            _exit(127);
        }
        // Parent: close child-side ends
        close(pipe_stdin[0]);
        close(pipe_stdout[1]);
        close(pipe_stderr[1]);
        g_child_pid = child_pid;
    }

    int read_from_child  = use_pty ? pty_master : pipe_stdout[0];
    int error_from_child = pipe_stderr[0];
    int write_to_child   = use_pty ? pty_master : pipe_stdin[1];

    int old_stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    fcntl(STDIN_FILENO,     F_SETFL, old_stdin_flags | O_NONBLOCK);
    fcntl(read_from_child,  F_SETFL, fcntl(read_from_child,  F_GETFL) | O_NONBLOCK);
    if (!use_pty)
        fcntl(error_from_child, F_SETFL, fcntl(error_from_child, F_GETFL) | O_NONBLOCK);

    bool child_out_closed = false;
    bool child_err_closed = use_pty;
    bool stdin_closed     = false;

    while (!child_out_closed || !child_err_closed) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (!stdin_closed)     FD_SET(STDIN_FILENO, &rfds);
        if (!child_out_closed) FD_SET(read_from_child, &rfds);
        if (!child_err_closed) FD_SET(error_from_child, &rfds);

        int maxfd = std::max({STDIN_FILENO, read_from_child, error_from_child}) + 1;
        int sel   = select(maxfd, &rfds, nullptr, nullptr, nullptr);

        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // ---- stdin → child ------------------------------------------------
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[BUFSIZE];
            ssize_t n = read(STDIN_FILENO, buf, BUFSIZE);
            if (n > 0) {
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
        if (FD_ISSET(read_from_child, &rfds))
            child_out_closed = proxy_child_output(read_from_child, STDOUT_FILENO);

        // ---- child stderr → our stderr ------------------------------------
        if (FD_ISSET(error_from_child, &rfds))
            child_err_closed = proxy_child_output(error_from_child, STDERR_FILENO);
    }

    restore_termios();
    fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags);

    int status = 0;
    waitpid(child_pid, &status, 0);
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

#endif  // _WIN32 / Unix

// ===========================================================================
// Entry point
// ===========================================================================

int main(int argc, char *argv[])
{
    if (argc < 2)
        return 0;

    std::vector<char *> child_argv(argv + 1, argv + argc);
    child_argv.push_back(nullptr);

#ifdef _WIN32
    return run_windows(child_argv);
#else
    return run_unix(argv[1], child_argv);
#endif
}
