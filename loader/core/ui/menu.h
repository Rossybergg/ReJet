/* The ReJet menu: an in-game pause menu drawn over the game (settings, bug reports), plus
 * on-screen messages and the frame-rate counter. Opened with F1, or Back + Start on any
 * controller. Settings it changes are saved to rejet.ini at once. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct IDirect3DDevice9Ex;

/* What the menu needs from the renderer (d3d.c). */
typedef struct MenuHost {
    struct IDirect3DDevice9Ex *dev;
    HWND wnd;
    void (*present)(void);                     /* show the back buffer */
    void (*request_capture)(const char *note); /* bug capture of the next frame, with a note */
    void (*apply_window)(void);                /* window size or full screen changed */
    void (*change_display)(void);              /* resolution or VSync changed (device reset) */
} MenuHost;

/* d3d.c: once the device exists. */
void menu_init(const MenuHost *host);

/* d3d.c, in Swap after screenshots and captures, before Present: opens the menu when asked
 * (then runs it here, the game paused, until it closes), and draws messages and the counter. */
void menu_frame(void);

/* Window thread: key and character messages. Returns 1 if the menu used the message. */
int menu_window_message(UINT msg, WPARAM wp);

/* A short on-screen message (printf format), e.g. "Screenshot saved". Any thread. */
void menu_toast(const char *fmt, ...);

/* The menu is open (the game is paused). */
int menu_is_open(void);

/* xpp.c: filter the game's view of a controller (Xbox button bits). Returns 1 when the game
 * should see nothing (menu open, or buttons still held after closing it). While Back is held,
 * Start is kept for the menu. */
int menu_filter_pad(unsigned port, unsigned short *buttons);

/* The ReJet logo (premultiplied BGRA), for the setup screen. Returns 0 if there is none. */
int menu_logo(const DWORD **px, UINT *w, UINT *h);
