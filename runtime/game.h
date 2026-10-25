/* Per-game names: this runtime is shared by the LEGO Batman and LEGO Marvel
 * ports and Skyrim. Build with -DGAME_MARVEL or -DGAME_SKYRIM (default is Batman). */
#pragma once
#if defined(GAME_SKYRIM)
#define GAME_EXE       "TESV.exe"
#define DLL_D3DX9      "d3dx9_42.dll"
#define DLL_EXTRA      "binkw32.dll"
#define GAME_WIN_DIR   "C:\\Skyrim"
#define GAME_WIN_PREFIX "\\skyrim\\"
#define GAME_TITLE     "Skyrim"
#define GAME_PKG_DIR   "com.tj.skyrim"
#elif defined(GAME_MARVEL)
#define GAME_EXE       "LEGOMARVEL.exe"
#define DLL_D3DX9      "d3dx9_43.dll"
#define DLL_EXTRA      "D3DCompiler_43.dll"
#define GAME_WIN_DIR   "C:\\LEGOMarvel"
#define GAME_WIN_PREFIX "\\legomarvel\\"
#define GAME_TITLE     "LEGO Marvel Super Heroes"
#define GAME_PKG_DIR   "com.tj.legomarvel"
#else
#define GAME_EXE       "LEGOBatman.exe"
#define DLL_D3DX9      "d3dx9_35.dll"
#define DLL_EXTRA      "binkw32.dll"
#define GAME_WIN_DIR   "C:\\LEGOBatman"
#define GAME_WIN_PREFIX "\\legobatman\\"
#define GAME_TITLE     "LEGO Batman"
#define GAME_PKG_DIR   "com.tj.legobatman"
#define GAME_BATMAN 1
#endif
