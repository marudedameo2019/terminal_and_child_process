#include <windows.h>
#include <iostream>
#include <vector>

int main() {
    using namespace std;
    CHAR s[] = "C:\\msys64\\usr\\bin\\sh.exe -c \"PATH=$PATH:/usr/bin ./child.sh\"";
    cout << s << endl;

    HANDLE hChildStdinC = NULL;
    HANDLE hChildStdinP = NULL;
    HANDLE hChildStdoutC = NULL;
    HANDLE hChildStdoutP = NULL;

    SECURITY_ATTRIBUTES sa; 
    sa.nLength = sizeof(SECURITY_ATTRIBUTES); 
    sa.bInheritHandle = TRUE; 
    sa.lpSecurityDescriptor = NULL; 

    if (! CreatePipe(&hChildStdinC, &hChildStdinP, &sa, 0)) return 1;
    if (! SetHandleInformation(hChildStdinP, HANDLE_FLAG_INHERIT, 0)) return 1;
    if (! CreatePipe(&hChildStdoutP, &hChildStdoutC, &sa, 0)) return 1;
    if (! SetHandleInformation(hChildStdoutP, HANDLE_FLAG_INHERIT, 0)) return 1;

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput = hChildStdinC;
    si.hStdOutput = hChildStdoutC;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    if (! CreateProcessA(NULL, s, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return 1;
    CloseHandle(hChildStdinC);
    CloseHandle(hChildStdoutC);

    vector<char> buff(4096);
    DWORD size;
    do {
        if (! ReadFile(hChildStdoutP, buff.data(), static_cast<DWORD>(buff.size()), &size, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            return 1;
        }
        cout.write(buff.data(), size);
        cout.flush();
    } while (size > 0);
    CloseHandle(hChildStdinP);
    
    if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0) return 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return 0;
}