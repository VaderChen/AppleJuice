/* 無 CRT 的 Windows x64 console 範例：由三個 kernel32 API 完成輸出與退出。 */
#include <windows.h>
#ifndef EXIT_CODE
#define EXIT_CODE 0
#endif
static const char message[] = "Hello from Windows x64 EXE on ARM64!\n";
void entry(void) {
    DWORD written = 0;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!WriteFile(output, message, sizeof(message) - 1, &written, NULL))
        ExitProcess(1);
    if (written != sizeof(message) - 1)
        ExitProcess(2);
    ExitProcess(EXIT_CODE);
}
