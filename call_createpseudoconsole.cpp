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
};

struct termseq {
    enum TYPE {
        NONE,
        STRING,
        SCI_C_PM_C,
        SCI_PM_C,
        WINDOW_TITLE,
        BEL,
        DECKPAM,
        DECKPNM,
        UNKNOWN
    } type;
    size_t offset;
    size_t length;
    union {
        struct {
            int ps1;
            int ps2;
            int ps3;
        } pm;
    };
};

#define W_NUL L'\0'
#define W_BEL L'\x07'
#define W_ESC L'\x1b'

wchar_t GetWC(const std::wstring& s, size_t offset) {
    if (offset < s.length()) return s[offset];
    else return W_NUL;
}

bool CompareWStr(const std::wstring& s, size_t offset, const std::wstring& r) {
    if (offset + r.length() > s.length()) return false;
    return wcsncmp(&s[offset], r.data(), r.length()) == 0;
}

size_t GetNumberFromWStr(const std::wstring& s, size_t offset, int* num) {
    int r = 0;
    size_t len = 0;
    wchar_t ch;
    while ((ch = GetWC(s, offset + len)) != W_NUL && ch >= L'0' && ch <= L'9') {
        r *= 10;
        r += ch - L'0';
        ++len;
    }
    *num = r;
    return len;
}

void AnalyzeSCI(const std::wstring &s, size_t offset, termseq &e) {
    int num[] = {0, 0, 0};
    size_t len = 0;
    size_t num_count = 0;
    for (num_count = 0; num_count < sizeof(num) / sizeof(num[0]); ++num_count) {
        size_t numlen = GetNumberFromWStr(s, offset + 2 + len, &num[num_count]);
        if (numlen == 0) break;
        len += numlen;
        if (GetWC(s, offset + 2 + len) != L';') break;
        ++len;
    }
    if (len == 0 && GetWC(s, offset + 2) == L'?') {
        len = GetNumberFromWStr(s, offset + 3, &num[0]);
        auto ch = GetWC(s, offset + 3 + len);
        if (ch == L'h' || ch == L'l') {
            if (len > 0) {
                e.type = termseq::SCI_C_PM_C;
                e.pm.ps1 = num[0];
                e.length = 4 + len;
            }
        }
    } else {
        switch (GetWC(s, offset + 2 + len)) {
        case L'J':
        case L'K':
        case L'm':
        case L'X':
        case L'C':
        case L't':
        case L'H':
            e.type = termseq::SCI_PM_C;
            e.length = 3 + len;
            e.pm.ps1 = num[0];
            e.pm.ps2 = num[1];
            e.pm.ps3 = num[2];
            break;
        }
    }
}

termseq AnalyzeSequence(const std::wstring &s, size_t offset)
{
    termseq e{termseq::UNKNOWN, offset, 0};
    switch(GetWC(s, offset)) {
    case W_ESC:
        switch(GetWC(s, offset+1)) {
        case L'[':
            AnalyzeSCI(s, offset, e);
            break;
        case L'=':
            e.type = termseq::DECKPAM;
            e.length = 2;
            break;
        case L'>':
            e.type = termseq::DECKPNM;
            e.length = 2;
            break;
        case L']':
            if (CompareWStr(s, offset + 2, L"0;")) {
                // 後ろのSTRINGは普通の文字列トークンとして抽出されるので注意
                e.type = termseq::WINDOW_TITLE;
                e.length = 4;   
            }
        }
        break;
    case W_BEL:
        e.type = termseq::BEL;
        e.length = 1;
        break;
    default:
        size_t i = offset;
        for (; i < s.length(); ++i) {
            auto ch = s[i];
            if (ch == W_NUL || ch == W_ESC || ch == W_BEL) break;
        }
        if (i == offset) e.type = termseq::NONE;
        else {
            e.type = termseq::STRING;
            e.offset = offset;
            e.length = i - offset;
        }
    }
    return e;
}

void LogReadString(const std::chrono::high_resolution_clock::time_point &ts, std::wofstream &ofs, const std::wstring &s) {
#if defined(LOGGING)
    using namespace std;
    using namespace std::chrono;

    auto te = high_resolution_clock::now();
    double sec = static_cast<double>(duration_cast<milliseconds>(te - ts).count()) / 1000;
    ofs << L"\n[" << sec << L"sec]read(" << s.length() << L"): " << hex;
    for (size_t i = 0; i < s.length(); ++i)
        ofs << setw(2) << setfill(L'0') << (int)s[i] << L" ";
    ofs << dec << endl << s;
    ofs.flush();
#endif
}

void LogToken(std::wofstream &ofs, const std::wstring &ws, const termseq &e) {
#if defined(LOGGING)
    ofs << L"type: " << e.type << L"\nlength: " << e.length << L"\noffset: " << e.offset << L"\ncontent: ";
    ofs.write(ws.data() + e.offset, e.length);
    ofs << std::endl;
#endif
}

void LogLine(std::wofstream &ofs, const std::wstring &ws) {
#if defined(LOGGING)
    ofs << ws << std::endl;
#endif
}

void communicate(void *args) {
    auto* pArgs = reinterpret_cast<arg_communicate*>(args);

    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(pArgs->hPtyInPipe), _O_WRONLY | _O_TEXT);
    FILE* ofp = _fdopen(fd, "w");

    using namespace std;
    using namespace std::chrono;
    vector<char> cbuff(4096);
    vector<wchar_t> buff(4096);
#if defined(LOGGING)
    wofstream ofs("hoge.log");
#else
    wofstream ofs("nul");
#endif
    static const vector<wstring> stoppers{L"--続きます--", L"-- More  --", L"-- More  -- ", L":"};
    static const vector<wstring> ends{L"(END)"};
    auto ts = high_resolution_clock::now();

    wcout << L"wait input from pipe..." << endl;

    DWORD dwRead = 1;
    while (dwRead > 0) {
        if (! ReadFile(pArgs->hPtyOutPipe, cbuff.data(), (DWORD)cbuff.size(), &dwRead, NULL)) {
            auto err = GetLastError();
            if ((err == ERROR_BROKEN_PIPE) || (err == ERROR_INVALID_HANDLE)) break;
            DIE();
        }
        if (dwRead == 0) break;

        DWORD dwWideRead = MultiByteToWideChar(CP_UTF8, 0, cbuff.data(), dwRead, buff.data(), (DWORD)buff.size());
        wstring ws(buff.data(), dwWideRead);

        LogReadString(ts, ofs, ws);

        bool detected = false;
        bool need_quit = false;
        LogLine(ofs, L"\nCreated wstring... length: " + to_wstring(ws.length()));
        for (size_t offset = 0; offset < ws.length();) {
            auto e = AnalyzeSequence(ws, offset);
            if (e.type == termseq::UNKNOWN) {
                cerr << "UNKNOWN sequence found!" << endl;
                exit(1);
            }
            LogToken(ofs, ws, e);

            if (e.type == termseq::STRING) {
                for (const auto& s: stoppers) {
                    if (e.length >= s.length() && CompareWStr(ws, e.offset + e.length - s.length(), s)) {
                        detected = true;
                        break;
                    }
                }
                if (! detected) {
                    for (const auto& s: ends) {
                        if (e.length >= s.length() && CompareWStr(ws, e.offset + e.length - s.length(), s)) {
                            detected = true;
                            need_quit = true;
                            break;
                        }
                    }
                }
            }
            offset += e.length;
        }

        wcout << ws;
        wcout.flush();
        if (detected) {
            LogLine(ofs, L"detected!");
            Sleep(1000);
            fputws(need_quit ? L"q" : L" ", ofp);
            fflush(ofp);
        }
    }
    LogLine(ofs, L"Exited read loop!");
    if (ofp) fclose(ofp);
    CloseHandle(pArgs->hPtyInPipe);
    CloseHandle(pArgs->hPtyOutPipe);
    wcout << L"Pipe thread finished..." << endl;
}

int main() {
    using namespace std;
    setlocale(LC_ALL,".UTF8");

    HRESULT res = E_UNEXPECTED;
    HANDLE hPtyIn = INVALID_HANDLE_VALUE;
    HANDLE hPtyInPipe = INVALID_HANDLE_VALUE;
    HANDLE hPtyOut = INVALID_HANDLE_VALUE;
    HANDLE hPtyOutPipe = INVALID_HANDLE_VALUE;

    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD consoleMode = 0;
    DWORD err = 0;

    if (! GetConsoleMode(hStdout, &consoleMode)) DIE();
    wcout << L"Console mode(stdout): " << consoleMode << L" -> " << (consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) << endl;
    if (! SetConsoleMode(hStdout, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) DIE();

    if (! CreatePipe(&hPtyIn, &hPtyInPipe, NULL, 0)) DIE();
    if (! CreatePipe(&hPtyOutPipe, &hPtyOut, NULL, 0)) DIE();

    COORD sizeConsole{};
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (! GetConsoleScreenBufferInfo(hStdout, &csbi)) DIE();
    sizeConsole.X = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    sizeConsole.Y = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

    HPCON hPty = INVALID_HANDLE_VALUE;
    if (CreatePseudoConsole(sizeConsole, hPtyIn, hPtyOut, 0, &hPty) != S_OK) DIE();
    CloseHandle(hPtyIn);
    CloseHandle(hPtyOut);

    arg_communicate args = {hPtyInPipe, hPtyOutPipe};
    HANDLE hThread = reinterpret_cast<HANDLE>(_beginthread(communicate, 0, &args));
    
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOEX);
    size_t sizeAttr;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sizeAttr);
    vector<char> v(sizeAttr);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(v.data());
    if (! InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &sizeAttr)) DIE();
    if (! UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPty, sizeof(HPCON), NULL, NULL)) DIE();
    PROCESS_INFORMATION pi;
    #define MSYS_BASE L"C:\\msys64"
    // wchar_t s[] = MSYS_BASE L"\\usr\\bin\\sh.exe -c \"PATH=$PATH:/usr/bin ./child_more.sh\"";
    // wchar_t s[] = MSYS_BASE L"\\usr\\bin\\sh.exe -c \"PATH=$PATH:/usr/bin ./child_winmore.sh\"";
    wchar_t s[] = MSYS_BASE L"\\usr\\bin\\sh.exe -c \"PATH=$PATH:/usr/bin ./child.sh\"";
    // wchar_t s[] = L"cmd";
    if (! CreateProcessW(NULL, s, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) DIE();
    
    DWORD rslt;
    constexpr DWORD timeout_ms = 10000;
    if (WaitForSingleObject(pi.hThread, timeout_ms) != WAIT_OBJECT_0) DIE();
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) DIE();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hPtyInPipe);
    CloseHandle(hPtyOutPipe);
    if ((rslt = WaitForSingleObject(hThread, timeout_ms)) != WAIT_OBJECT_0) DIE();
    CloseHandle(hThread);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    ClosePseudoConsole(hPty);
    cerr << "Main thread finished..." << endl;

    return 0;
}