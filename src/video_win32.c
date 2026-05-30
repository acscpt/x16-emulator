// All rights reserved. License: 2-clause BSD

#include <SDL.h>
#include <SDL_syswm.h>

#include <windows.h>
#include <dwmapi.h>

void video_win32_set_rounded_corners(SDL_Window *window)
{
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	SDL_GetWindowWMInfo(window, &wmInfo);

	HWND hwnd = wmInfo.info.win.window;

	// Windows 11 rounded corners. DWMWA_WINDOW_CORNER_PREFERENCE and
	// DWMWCP_ROUNDSMALL are Windows 11 dwmapi additions; mingw-w64 w32api
	// headers older than v10 lack them, so fall back to their literal values.
	// The call is ignored on pre-Windows-11 systems. DWORD is used in place
	// of the (also-missing) DWM_WINDOW_CORNER_PREFERENCE typedef.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUNDSMALL
#define DWMWCP_ROUNDSMALL 3
#endif
	DWORD preference = DWMWCP_ROUNDSMALL;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}
