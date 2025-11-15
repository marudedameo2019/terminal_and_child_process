#include <iostream>
#include <cstdlib>

int main() {
    using namespace std;
    char s[] = "C:\\msys64\\usr\\bin\\sh.exe -c \"PATH=$PATH:/usr/bin ./child.sh\"";
    cout << s << endl;
    system(s);
    return 0;
}