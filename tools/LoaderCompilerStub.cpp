// Test fixture only, never distribute as a runtime dependency.
// Deliberately omits D3DCompile to reproduce an incompatible compiler DLL.
extern "C" __declspec(dllexport) int nSkinzCompilerFixture() { return 0; }
