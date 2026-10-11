// legacy-plugin-stub.c - the empty plugins\KexSuite\KexSuite.dll in the package. The KEX suite used to
// be that plugin; it is built into notepad++.exe now and Notepad++ KEX never loads a KexSuite.dll.
// The file only exists because the update script on PCs installed before the move copies it and
// stops if it is missing. The current Install-KEX.ps1 deletes plugins\KexSuite instead.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}
