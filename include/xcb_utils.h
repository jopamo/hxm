#ifndef XCB_UTILS_H
#define XCB_UTILS_H

#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>

// Atoms cache - all atoms we will ever touch
struct atoms {
    xcb_atom_t WM_PROTOCOLS;
    xcb_atom_t WM_DELETE_WINDOW;
    xcb_atom_t WM_TAKE_FOCUS;
    xcb_atom_t _NET_WM_PING;
    xcb_atom_t WM_STATE;
    xcb_atom_t WM_CLASS;
    xcb_atom_t WM_CLIENT_MACHINE;
    xcb_atom_t WM_COLORMAP_WINDOWS;
    xcb_atom_t WM_COMMAND;
    xcb_atom_t WM_NAME;
    xcb_atom_t WM_ICON_NAME;
    xcb_atom_t WM_HINTS;
    xcb_atom_t WM_NORMAL_HINTS;
    xcb_atom_t WM_TRANSIENT_FOR;
    xcb_atom_t WM_CHANGE_STATE;
    xcb_atom_t _MOTIF_WM_HINTS;
    xcb_atom_t _GTK_FRAME_EXTENTS;
    xcb_atom_t _NET_WM_SYNC_REQUEST;

    xcb_atom_t _NET_SUPPORTED;
    xcb_atom_t _NET_CLIENT_LIST;
    xcb_atom_t _NET_CLIENT_LIST_STACKING;
    xcb_atom_t _NET_ACTIVE_WINDOW;

    xcb_atom_t _NET_WM_NAME;
    xcb_atom_t _NET_WM_VISIBLE_NAME;
    xcb_atom_t _NET_WM_ICON_NAME;
    xcb_atom_t _NET_WM_VISIBLE_ICON_NAME;
    xcb_atom_t _NET_WM_STATE;
    xcb_atom_t _NET_WM_WINDOW_TYPE;
    xcb_atom_t _NET_WM_STRUT;
    xcb_atom_t _NET_WM_STRUT_PARTIAL;
    xcb_atom_t _NET_WORKAREA;
    xcb_atom_t _NET_WM_PID;

    xcb_atom_t _NET_WM_USER_TIME;
    xcb_atom_t _NET_WM_USER_TIME_WINDOW;
    xcb_atom_t _NET_WM_SYNC_REQUEST_COUNTER;
    xcb_atom_t _NET_WM_ICON_GEOMETRY;

    xcb_atom_t _NET_WM_STATE_FULLSCREEN;
    xcb_atom_t _NET_WM_STATE_ABOVE;
    xcb_atom_t _NET_WM_STATE_BELOW;
    xcb_atom_t _NET_WM_STATE_STICKY;
    xcb_atom_t _NET_WM_STATE_DEMANDS_ATTENTION;
    xcb_atom_t _NET_WM_STATE_HIDDEN;
    xcb_atom_t _NET_WM_STATE_MAXIMIZED_HORZ;
    xcb_atom_t _NET_WM_STATE_MAXIMIZED_VERT;
    xcb_atom_t _NET_WM_STATE_FOCUSED;
    xcb_atom_t _NET_WM_STATE_MODAL;
    xcb_atom_t _NET_WM_STATE_SHADED;
    xcb_atom_t _NET_WM_STATE_SKIP_TASKBAR;
    xcb_atom_t _NET_WM_STATE_SKIP_PAGER;

    xcb_atom_t _NET_WM_WINDOW_TYPE_DOCK;
    xcb_atom_t _NET_WM_WINDOW_TYPE_DIALOG;
    xcb_atom_t _NET_WM_WINDOW_TYPE_NOTIFICATION;
    xcb_atom_t _NET_WM_WINDOW_TYPE_NORMAL;
    xcb_atom_t _NET_WM_WINDOW_TYPE_DESKTOP;
    xcb_atom_t _NET_WM_WINDOW_TYPE_SPLASH;
    xcb_atom_t _NET_WM_WINDOW_TYPE_TOOLBAR;
    xcb_atom_t _NET_WM_WINDOW_TYPE_UTILITY;
    xcb_atom_t _NET_WM_WINDOW_TYPE_MENU;
    xcb_atom_t _NET_WM_WINDOW_TYPE_DROPDOWN_MENU;
    xcb_atom_t _NET_WM_WINDOW_TYPE_POPUP_MENU;
    xcb_atom_t _NET_WM_WINDOW_TYPE_TOOLTIP;
    xcb_atom_t _NET_WM_WINDOW_TYPE_COMBO;
    xcb_atom_t _NET_WM_WINDOW_TYPE_DND;

    xcb_atom_t _NET_SUPPORTING_WM_CHECK;
    xcb_atom_t _NET_DESKTOP_VIEWPORT;
    xcb_atom_t _NET_NUMBER_OF_DESKTOPS;
    xcb_atom_t _NET_CURRENT_DESKTOP;
    xcb_atom_t _NET_VIRTUAL_ROOTS;
    xcb_atom_t _NET_DESKTOP_NAMES;
    xcb_atom_t _NET_WM_DESKTOP;

    xcb_atom_t _NET_WM_ICON;
    xcb_atom_t _NET_CLOSE_WINDOW;
    xcb_atom_t _NET_DESKTOP_GEOMETRY;
    xcb_atom_t _NET_FRAME_EXTENTS;
    xcb_atom_t _NET_REQUEST_FRAME_EXTENTS;
    xcb_atom_t _NET_SHOWING_DESKTOP;
    xcb_atom_t _NET_WM_WINDOW_OPACITY;

    xcb_atom_t _NET_WM_ALLOWED_ACTIONS;
    xcb_atom_t _NET_WM_ACTION_MOVE;
    xcb_atom_t _NET_WM_ACTION_RESIZE;
    xcb_atom_t _NET_WM_ACTION_MINIMIZE;
    xcb_atom_t _NET_WM_ACTION_SHADE;
    xcb_atom_t _NET_WM_ACTION_STICK;
    xcb_atom_t _NET_WM_ACTION_MAXIMIZE_HORZ;
    xcb_atom_t _NET_WM_ACTION_MAXIMIZE_VERT;
    xcb_atom_t _NET_WM_ACTION_FULLSCREEN;
    xcb_atom_t _NET_WM_ACTION_CHANGE_DESKTOP;
    xcb_atom_t _NET_WM_ACTION_CLOSE;
    xcb_atom_t _NET_WM_ACTION_ABOVE;
    xcb_atom_t _NET_WM_ACTION_BELOW;
    xcb_atom_t _NET_WM_MOVERESIZE;
    xcb_atom_t _NET_MOVERESIZE_WINDOW;
    xcb_atom_t _NET_RESTACK_WINDOW;
    xcb_atom_t _NET_WM_FULLSCREEN_MONITORS;
    xcb_atom_t _NET_WM_FULL_PLACEMENT;

    xcb_atom_t UTF8_STRING;
    xcb_atom_t COMPOUND_TEXT;
    xcb_atom_t WM_S0;
    xcb_atom_t _NET_WM_BYPASS_COMPOSITOR;
};

extern struct atoms atoms;

// Initialize connection and cache atoms
xcb_connection_t* xcb_connect_cached(void);
void atoms_init(xcb_connection_t* conn);
void atoms_print(void);

xcb_visualtype_t* xcb_get_visualtype(xcb_connection_t* conn, xcb_visualid_t visual_id);

#endif  // XCB_UTILS_H
