#pragma once
// The program's one Win32 child-process launcher.
//
// Everything SmackMyRezUp runs out of process - ffmpeg, ffprobe, the headless
// exporter, the Python sidecars - starts here, so the fiddly parts live in one
// audited place: which standard stream is a pipe, which end of that pipe the
// parent keeps, and closing the child's end straight after the spawn so a
// reader eventually sees EOF and a writer eventually sees a broken pipe.
//
// A stream the caller does not ask for is wired to NUL rather than left unset.
// The player is a GUI program with no console, so its children used to inherit
// null standard handles and could fail on their first write; NUL swallows that
// output instead. Stdio::Console asks for the parent's own handle and falls
// back to NUL when there is none, so the same call works from both executables.
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace smru::proc {

// Windows filenames cannot contain a literal quote, so wrapping is enough for
// the executable and media paths this program puts on a command line.
inline std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// How one of the child's three standard streams is wired.
enum class Stdio {
    Null,     // the NUL device: writes vanish, reads see EOF
    Pipe,     // a pipe whose other end stays in Child::stdIn / Child::stdOut
    Console,  // the parent's own handle, or NUL when the parent has no console
};

struct Options {
    Stdio in  = Stdio::Null;
    Stdio out = Stdio::Null;
    Stdio err = Stdio::Null;   // Stdio::Pipe here merges stderr into the stdout pipe
    DWORD pipeBytes = 1 << 16; // buffer for whichever streams are pipes
    const wchar_t* workingDir = nullptr;
};

// The parent's side of a running child. stdIn and stdOut are non-null exactly
// when the matching Options field asked for a pipe; the caller owns all three
// handles and releases them with Close().
struct Child {
    HANDLE process = nullptr;
    HANDLE stdIn   = nullptr;  // write end, feeding the child's stdin
    HANDLE stdOut  = nullptr;  // read end, draining the child's stdout (+stderr)
    explicit operator bool() const { return process != nullptr; }
};

namespace detail {

inline void Close(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = nullptr;
}

inline HANDLE OpenNul(SECURITY_ATTRIBUTES& sa, DWORD access) {
    const HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

} // namespace detail

inline void Close(Child& c) {
    detail::Close(c.stdIn);
    detail::Close(c.stdOut);
    detail::Close(c.process);
}

// Starts commandLine (a full, self-quoting command line - there is no separate
// application name, so the first token must be quoted when it contains spaces)
// with no console window. Returns false when nothing was started, in which case
// child stays empty and every handle this created is already closed.
inline bool Spawn(const std::wstring& commandLine, const Options& opt, Child& child) {
    child = {};
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };

    HANDLE inRead = nullptr, inWrite = nullptr;    // child reads inRead, parent writes inWrite
    HANDLE outRead = nullptr, outWrite = nullptr;  // child writes outWrite, parent reads outRead
    HANDLE nulRead = nullptr, nulWrite = nullptr;
    auto fail = [&] {
        detail::Close(inRead);  detail::Close(inWrite);
        detail::Close(outRead); detail::Close(outWrite);
        detail::Close(nulRead); detail::Close(nulWrite);
        child = {};
        return false;
    };

    if (opt.in == Stdio::Pipe) {
        if (!CreatePipe(&inRead, &inWrite, &sa, opt.pipeBytes)) return fail();
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0); // the parent's end stays home
    }
    if (opt.out == Stdio::Pipe || opt.err == Stdio::Pipe) {
        if (!CreatePipe(&outRead, &outWrite, &sa, opt.pipeBytes)) return fail();
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;

    auto inherited = [&](DWORD id) {
        const HANDLE h = GetStdHandle(id);
        return (h && h != INVALID_HANDLE_VALUE) ? h : nullptr;
    };
    if (opt.in == Stdio::Pipe) {
        si.hStdInput = inRead;
    } else if (opt.in == Stdio::Console && inherited(STD_INPUT_HANDLE)) {
        si.hStdInput = inherited(STD_INPUT_HANDLE);
    } else {
        nulRead = detail::OpenNul(sa, GENERIC_READ);
        if (!nulRead) return fail();
        si.hStdInput = nulRead;
    }
    auto sink = [&](Stdio kind, DWORD id) -> HANDLE {
        if (kind == Stdio::Pipe) return outWrite;
        if (kind == Stdio::Console && inherited(id)) return inherited(id);
        if (!nulWrite) nulWrite = detail::OpenNul(sa, GENERIC_WRITE);
        return nulWrite;
    };
    si.hStdOutput = sink(opt.out, STD_OUTPUT_HANDLE);
    si.hStdError  = sink(opt.err, STD_ERROR_HANDLE);
    if (!si.hStdOutput || !si.hStdError) return fail();

    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, opt.workingDir, &si, &pi);

    // The child has its own copies now; holding on to the ends it owns is what
    // makes a drain loop hang forever waiting for an EOF that never arrives.
    detail::Close(inRead);
    detail::Close(outWrite);
    detail::Close(nulRead);
    detail::Close(nulWrite);
    if (!ok) {
        detail::Close(inWrite);
        detail::Close(outRead);
        return false;
    }
    CloseHandle(pi.hThread);
    child.process = pi.hProcess;
    child.stdIn = inWrite;
    child.stdOut = outRead;
    return true;
}

struct CaptureOptions {
    DWORD timeoutMs = INFINITE;  // a hard limit: the child is killed when it expires
    bool captureStderr = true;   // false sends the child's stderr to NUL instead
    const wchar_t* workingDir = nullptr;
};

// Runs a command to completion and returns its output bytes and exit code.
// Returns false only when the child could not be started; a child that fails or
// is killed on the timeout still reports whatever it managed to write.
inline bool RunCapture(const std::wstring& commandLine, std::string& output, DWORD& exitCode,
                       const CaptureOptions& capture = {}) {
    output.clear();
    exitCode = 0;
    Options opt;
    opt.out = Stdio::Pipe;
    opt.err = capture.captureStderr ? Stdio::Pipe : Stdio::Null;
    opt.workingDir = capture.workingDir;
    Child child;
    if (!Spawn(commandLine, opt, child)) return false;

    // Drained as it arrives: a child that fills the pipe buffer blocks on write,
    // which would deadlock a parent that only reads once the child has exited.
    auto drain = [&] {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(child.stdOut, nullptr, 0, nullptr, &avail, nullptr) || !avail) return;
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(child.stdOut, buf, std::min<DWORD>(avail, DWORD(sizeof(buf))), &got, nullptr) || !got) return;
            output.append(buf, got);
        }
    };
    const ULONGLONG deadline = GetTickCount64() + capture.timeoutMs;
    for (;;) {
        drain();
        if (WaitForSingleObject(child.process, 50) == WAIT_OBJECT_0) break;
        if (capture.timeoutMs != INFINITE && GetTickCount64() > deadline) {
            TerminateProcess(child.process, 1);
            WaitForSingleObject(child.process, 1000);
            break;
        }
    }
    drain();
    GetExitCodeProcess(child.process, &exitCode);
    Close(child);
    return true;
}

} // namespace smru::proc
