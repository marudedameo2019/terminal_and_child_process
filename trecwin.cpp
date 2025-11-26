#define WINVER 0x0A00 // Windows 10以降のSDKを使用
#define _WIN32_WINNT 0x0A00 

#include <windows.h>
#include <process.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <memory>
#include <io.h>
#include <fcntl.h>

#if defined(_MSC_VER)
#define FUNC __FUNCSIG__
#else
#define FUNC __PRETTY_FUNCTION__
#endif

#define DIE() die(__FILE__, __LINE__, FUNC)

std::string GetMessageString(DWORD err) {
    std::vector<char> buff(2048); 
    DWORD dwLen = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buff.data(), (DWORD)buff.size(), NULL);
    return "[code=" + std::to_string(err) + "]" + std::string(buff.data(), buff.size());
}

void die(const char* s, size_t n, const char* f) {
    auto err = GetLastError();
    std::cerr << s << ":" << n << ":" << f << ": " << GetMessageString(err) << std::endl;
    exit(1);
}

struct arg_communicate {
    HANDLE hPtyInPipe;
    HANDLE hPtyOutPipe;
    std::string sFnameScriptOut;
    std::string sFnameTiming;
};

HANDLE hStdin = INVALID_HANDLE_VALUE;
HANDLE hStdout = INVALID_HANDLE_VALUE;
HANDLE hStderr = INVALID_HANDLE_VALUE;
COORD sizeConsole{};

OVERLAPPED stdinOL;
volatile bool stopRequest = false;
CRITICAL_SECTION sec;

std::wofstream logging;

void log(const std::wstring& s) {
    EnterCriticalSection(&sec);
    logging << s << std::endl;
    LeaveCriticalSection(&sec);
}

DWORD communicate_out(void *args) {
    auto* pArgs = reinterpret_cast<arg_communicate*>(args);

    using namespace std;
    using namespace std::chrono;
    vector<char> cbuff(4096);
    vector<wchar_t> buff(4096);
    auto ts = high_resolution_clock::now();
    
    log(L"[PipeThreadOut]wait input from pipe...");
    ofstream script(pArgs->sFnameScriptOut, ios::out | ios::binary);
    ofstream timing(pArgs->sFnameTiming, ios::out | ios::binary);

    DWORD dwRead = 1;
    script << "Script started on 2025-01-01 00:00:00+09:00 [TERM=\"xterm-256color\" TTY=\"/dev/pty0\" COLUMNS=\"" << sizeConsole.X << "\" LINES=\"" << sizeConsole.Y << "\"]" << endl;
    while (dwRead > 0) {
        dwRead = 0;
        if (! ReadFile(pArgs->hPtyOutPipe, cbuff.data(), (DWORD)cbuff.size(), &dwRead, NULL)) {
            auto err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) break;
            DIE();
        }
        if (dwRead == 0) break;

        auto te = high_resolution_clock::now();
        auto sec = static_cast<double>(duration_cast<microseconds>(high_resolution_clock::now() - ts).count())/1000000;
        ts = te;
        script.write(cbuff.data(), dwRead);
        script.flush();
        timing << fixed << setw(8) << setfill('0') << sec << " " << dwRead << endl;

        cout.write(cbuff.data(), dwRead);
        cout.flush();

        auto wlen = MultiByteToWideChar(CP_UTF8, 0, cbuff.data(), dwRead, buff.data(), (DWORD)buff.size()-1);
        buff[wlen] = L'\0';
        log(L"[PipeThreadOut]Read from pipe...");
        log(buff.data());
    }
    script << "\nScript done";
    script.flush();
    CloseHandle(pArgs->hPtyOutPipe);
    log(L"[PipeThreadOut]finished...");
    return 0;
}

DWORD communicate_in(void *args) {
    auto* pArgs = reinterpret_cast<arg_communicate*>(args);

    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(pArgs->hPtyInPipe), _O_WRONLY | _O_BINARY);
    FILE* ofp = _fdopen(fd, "wb");

    using namespace std;
    using namespace std::chrono;
    vector<char> cbuff(4096), u8buff(4097*3); 
    vector<wchar_t> buff(4097);
    auto ts = high_resolution_clock::now();

    log(L"[PipeThreadIn]wait stdin...");

    DWORD dwRead = 0;
    while (! stopRequest) {
        log(L"[PipeThreadIn]GetOverlappedResult(): before ReadFile()...");
        if (! ReadFile(hStdin, cbuff.data(), (DWORD)cbuff.size(), NULL, &stdinOL)) {
            auto err = GetLastError();
            if (err != ERROR_IO_PENDING && err != ERROR_OPERATION_ABORTED) DIE();
        }
        log(L"[PipeThreadIn]GetOverlappedResult(): before GetOverlappedResult()...");
        if (! GetOverlappedResult(hStdin, &stdinOL, &dwRead, TRUE)) {
            auto err = GetLastError();
            log(L"[PipeThreadIn]GetOverlappedResult(): after GetOverlappedResult()...");
            if (err != ERROR_IO_PENDING && err != ERROR_OPERATION_ABORTED) DIE();
            continue;
        }
        if (dwRead == 0) break;
        log(L"[PipeThreadIn]read!");
        auto wlen = MultiByteToWideChar(GetConsoleCP(), 0, cbuff.data(), dwRead, buff.data(), (DWORD)buff.size());
        auto u8len = WideCharToMultiByte(CP_UTF8, 0, buff.data(), wlen, u8buff.data(), (DWORD)u8buff.size(), NULL, NULL);
        fwrite(u8buff.data(), u8len, 1, ofp);
        fflush(ofp);
        log(buff.data());
    }
    if (ofp) fclose(ofp);
    CloseHandle(pArgs->hPtyInPipe);
    log(L"[PipeThreadIn]finished...");
    return 0;
}

#define STDOUT_FILE "typescript"
#define TIMING_FILE "file.tm"
#define LOG_FILE "nul"

int record(char* cmdline) {
    using namespace std;
    HANDLE hPtyIn = INVALID_HANDLE_VALUE;
    HANDLE hPtyInPipe = INVALID_HANDLE_VALUE;
    HANDLE hPtyOut = INVALID_HANDLE_VALUE;
    HANDLE hPtyOutPipe = INVALID_HANDLE_VALUE;

    hStdin = GetStdHandle(STD_INPUT_HANDLE);
    hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    hStderr = GetStdHandle(STD_ERROR_HANDLE);
    memset(&stdinOL, 0, sizeof(stdinOL));
    InitializeCriticalSection(&sec);

    logging.open(LOG_FILE);

    DWORD consoleMode = 0;
    if (! GetConsoleMode(hStdin, &consoleMode)) DIE();
    auto newConsoleMode = consoleMode;
    newConsoleMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    newConsoleMode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    wcout << L"Console mode(stdin): " << hex << "0x" << consoleMode << L" -> " << "0x" << newConsoleMode << dec << endl;
    if (! SetConsoleMode(hStdin, newConsoleMode)) DIE();
    if (! GetConsoleMode(hStdout, &consoleMode)) DIE();
    newConsoleMode = consoleMode;
    newConsoleMode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    wcout << L"Console mode(stdout): " << hex << "0x" << consoleMode << L" -> " << "0x" << newConsoleMode << dec << endl;
    if (! SetConsoleMode(hStdout, newConsoleMode)) DIE();

    if (! CreatePipe(&hPtyIn, &hPtyInPipe, NULL, 0)) DIE();
    if (! CreatePipe(&hPtyOutPipe, &hPtyOut, NULL, 0)) DIE();

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (! GetConsoleScreenBufferInfo(hStdout, &csbi)) DIE();
    sizeConsole.X = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    sizeConsole.Y = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

    HPCON hPty = INVALID_HANDLE_VALUE;
    if (CreatePseudoConsole(sizeConsole, hPtyIn, hPtyOut, 0, &hPty) != S_OK) DIE();
    CloseHandle(hPtyIn);
    CloseHandle(hPtyOut);

    arg_communicate args = {hPtyInPipe, hPtyOutPipe, STDOUT_FILE, TIMING_FILE};

    // _beginthread()は自動で閉じるしexを使ってもHANDLEは綺麗に扱えないので
    // 2つしかスレッドを作らないこのコードではCreateThreadを直に使う
    HANDLE hOutThread = CreateThread(NULL, 0, communicate_out, &args, 0, NULL);
    if (hOutThread == INVALID_HANDLE_VALUE) DIE();
    HANDLE hInThread = CreateThread(NULL, 0, communicate_in, &args, 0, NULL);
    if (hInThread == INVALID_HANDLE_VALUE) DIE();
    
    STARTUPINFOEXA si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOEXA);
    size_t sizeAttr;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sizeAttr);
    vector<char> v(sizeAttr);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(v.data());
    if (! InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &sizeAttr)) DIE();
    if (! UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPty, sizeof(HPCON), NULL, NULL)) DIE();
    PROCESS_INFORMATION pi;
    if (! CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) DIE();
    if (! WaitForInputIdle(pi.hProcess, INFINITE)) DIE();

    DWORD rslt;
    constexpr DWORD timeout_ms = INFINITE; //10000;
    if (WaitForSingleObject(pi.hThread, timeout_ms) != WAIT_OBJECT_0) DIE();
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) DIE();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log(L"[MainThread]calling CancelIoEx(hStdin)...");
    stopRequest = TRUE;
    CancelIoEx(hStdin, &stdinOL);
    if ((rslt = WaitForSingleObject(hInThread, timeout_ms)) != WAIT_OBJECT_0) DIE();
    CloseHandle(hInThread);
    // 標準出力側のパイプに読み込ませる/BROKEN PIPEするには疑似コンソールのCloseが必要
    ClosePseudoConsole(hPty);
    log(L"[MainThread]hPty closed...");
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if ((rslt = WaitForSingleObject(hOutThread, timeout_ms)) != WAIT_OBJECT_0) DIE();
    CloseHandle(hOutThread);
    log(L"[MainThread]Main thread finished...");
    return 0;
}

int play() {
    using namespace std;
    ifstream timing(TIMING_FILE);
    ifstream out(STDOUT_FILE, ios::in | ios::binary);
    double time;
    size_t len;
    vector<char> buff(4096);
    string s;
    getline(out, s);
    while (timing >> time) {
        timing >> len;
        Sleep((DWORD)(time*1000));
        out.read(buff.data(), len);
        cout.write(buff.data(), len);
        cout.flush();
    }
    return 0;
}

int main(int argc, char* argv[]) {
    setlocale(LC_ALL,".UTF8");
    #define MSYS_BASE "C:\\msys64"
    // char s[] = "cmd";
    char s[] = "powershell";
    // char s[] = MSYS_BASE "\\msys2_shell.cmd -ucrt64 -defterm -no-start -here";
    char* cmdline = (argc > 1) ? argv[1] : s;
    const std::string strPlay("--play");
    if (argc > 1) {
        if (strPlay == argv[1]) {
            return play();
        }
    }
    return record(cmdline);
}