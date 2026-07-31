#include <windows.h>

#ifndef FLS_D3D12_AGILITY_VERSION
#error FLS_D3D12_AGILITY_VERSION must be supplied by the opted-in Windows build
#endif

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = FLS_D3D12_AGILITY_VERSION;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
