/*
 *  OpenVPN-GUI -- A Windows GUI for OpenVPN.
 *
 *  Copyright (C) 2017 Selva Nair <selva.nair@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <windows.h>
#include <wchar.h>
#include <richedit.h>
#include "main.h"
#include "options.h"
#include "misc.h"
#include "openvpn.h"
#include "echo.h"
#include "tray.h"
#include "openvpn-gui-res.h"
#include "localization.h"

extern options_t o;

/* echo msg types */
#define ECHO_MSG_WINDOW (1)
#define ECHO_MSG_NOTIFY (2)

struct echo_msg_history {
    struct echo_msg_fp fp;
    struct echo_msg_history *next;
};

/* We use a global message window for all messages
 */
static HWND echo_msg_window;

/* Forward declarations */
static void
AddMessageBoxText(HWND hwnd, const wchar_t *text, const wchar_t *title, BOOL show);
static INT_PTR CALLBACK
MessageDialogFunc(HWND hwnd, UINT msg, UNUSED WPARAM wParam, LPARAM lParam);

void
echo_msg_init(void)
{
    echo_msg_window = CreateLocalizedDialogParam(ID_DLG_MESSAGE, MessageDialogFunc, (LPARAM) 0);

    if (!echo_msg_window)
    {
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"Error creating echo message window.");
    }
}

/* compute a digest of the message and add it to the msg struct */
static void
echo_msg_add_fp(struct echo_msg *msg, time_t timestamp)
{
    md_ctx ctx;

    msg->fp.timestamp = timestamp;
    if (md_init(&ctx, CALG_SHA1) != 0)
        return;
    md_update(&ctx, (BYTE*) msg->text, msg->txtlen*sizeof(msg->text[0]));
    md_update(&ctx, (BYTE*) msg->title, wcslen(msg->title)*sizeof(msg->title[0]));
    md_final(&ctx, msg->fp.digest);
    return;
}

/* find message with given digest in history */
static struct echo_msg_history *
echo_msg_recall(const BYTE *digest, struct echo_msg_history *hist)
{
    for( ; hist; hist = hist->next)
    {
        if (memcmp(hist->fp.digest, digest, HASHLEN) == 0) break;
    }
    return hist;
}

/* Add an item to message history and return the head of the list */
static struct echo_msg_history*
echo_msg_history_add(struct echo_msg_history *head, const struct echo_msg_fp *fp)
{
    struct echo_msg_history *hist = malloc(sizeof(struct echo_msg_history));
    if (hist)
    {
        memcpy(&hist->fp, fp, sizeof(*fp));
        hist->next = head;
        head = hist;
    }
    return head;
}

/* Save message in history -- update if already present */
static void
echo_msg_save(struct echo_msg *msg)
{
    struct echo_msg_history *hist = echo_msg_recall(msg->fp.digest, msg->history);
    if (hist) /* update */
    {
        hist->fp.timestamp = msg->fp.timestamp;
    }
    else     /* add */
    {
        msg->history = echo_msg_history_add(msg->history, &msg->fp);
    }
}

/* persist echo msg history to the registry */
void
echo_msg_persist(connection_t *c)
{
    /* Not implemented */
    return;
}

/* load echo msg history from registry */
void
echo_msg_load(connection_t *c)
{
    /* Not implemented */
    return;
}

/* Return true if the message is same as recently shown */
static BOOL
echo_msg_repeated(const struct echo_msg *msg)
{
    const struct echo_msg_history *hist;

    hist = echo_msg_recall(msg->fp.digest, msg->history);
    return (hist && (hist->fp.timestamp + o.popup_mute_interval*3600 > msg->fp.timestamp));
}

/* Append a line of echo msg */
static void
echo_msg_append(connection_t *c, time_t UNUSED timestamp, const char *msg, BOOL addnl)
{
    wchar_t *eol = L"";
    wchar_t *wmsg = NULL;

    if (!(wmsg = Widen(msg)))
    {
        WriteStatusLog(c, L"GUI> ", L"Error: out of memory while processing echo msg", false);
        goto out;
    }

    size_t len = c->echo_msg.txtlen + wcslen(wmsg) + 1;  /* including null terminator */
    if (addnl)
    {
        eol = L"\r\n";
        len += 2;
    }
    WCHAR *s = realloc(c->echo_msg.text, len*sizeof(WCHAR));
    if (!s)
    {
        WriteStatusLog(c, L"GUI> ", L"Error: out of memory while processing echo msg", false);
        goto out;
    }
    swprintf(s + c->echo_msg.txtlen, len - c->echo_msg.txtlen,  L"%s%s", wmsg, eol);

    s[len-1] = L'\0';
    c->echo_msg.text = s;
    c->echo_msg.txtlen = len - 1; /* exclude null terminator */

out:
    free(wmsg);
    return;
}

/* Called when echo msg-window or echo msg-notify is received */
static void
echo_msg_display(connection_t *c, time_t timestamp, const char *title, int type)
{
    WCHAR *wtitle = Widen(title);

    if (wtitle)
    {
        c->echo_msg.title = wtitle;
    }
    else
    {
        WriteStatusLog(c, L"GUI> ", L"Error: out of memory converting echo message title to widechar", false);
        c->echo_msg.title = L"Message from server";
    }
    echo_msg_add_fp(&c->echo_msg, timestamp); /* add fingerprint: digest+timestamp */

     /* Check whether the message is muted */
    if (c->flags & FLAG_DISABLE_ECHO_MSG || echo_msg_repeated(&c->echo_msg))
    {
        return;
    }
    if (type == ECHO_MSG_WINDOW)
    {
        HWND h = echo_msg_window;
        if (h)
        {
            AddMessageBoxText(h, c->echo_msg.text, c->echo_msg.title, true);
        }
    }
    else /* notify */
    {
        ShowTrayBalloon(c->echo_msg.title, c->echo_msg.text);
    }
    /* save or update history */
    echo_msg_save(&c->echo_msg);
}

void
echo_msg_process(connection_t *c, time_t timestamp, const char *s)
{
    wchar_t errmsg[256] = L"";

    char *msg = url_decode(s);
    if (!msg)
    {
        WriteStatusLog(c, L"GUI> ", L"Error in url_decode of echo message", false);
        return;
    }

    if (strbegins(msg, "msg "))
    {
        echo_msg_append(c, timestamp, msg + 4, true);
    }
    else if (streq(msg, "msg")) /* empty msg is treated as a new line */
    {
        echo_msg_append(c, timestamp, msg+3, true);
    }
    else if (strbegins(msg, "msg-n "))
    {
        echo_msg_append(c, timestamp, msg + 6, false);
    }
    else if (strbegins(msg, "msg-window "))
    {
        echo_msg_display(c, timestamp, msg + 11, ECHO_MSG_WINDOW);
        echo_msg_clear(c, false);
    }
    else if (strbegins(msg, "msg-notify "))
    {
        echo_msg_display(c, timestamp, msg + 11, ECHO_MSG_NOTIFY);
        echo_msg_clear(c, false);
    }
    else
    {
        _sntprintf_0(errmsg, L"WARNING: Unknown ECHO directive '%hs' ignored.", msg);
        WriteStatusLog(c, L"GUI> ", errmsg, false);
    }
    free(msg);
}

void
echo_msg_clear(connection_t *c, BOOL clear_history)
{
    CLEAR(c->echo_msg.fp);
    free(c->echo_msg.text);
    free(c->echo_msg.title);
    c->echo_msg.text = NULL;
    c->echo_msg.txtlen = 0;
    c->echo_msg.title = NULL;

    if (clear_history)
    {
        echo_msg_persist(c);
        struct echo_msg_history *head = c->echo_msg.history;
        struct echo_msg_history *next;
        while (head)
        {
            next = head->next;
            free(head);
            head = next;
        }
        CLEAR(c->echo_msg);
    }
}

/* Add new message to the message box window and optionally show it */
static void
AddMessageBoxText(HWND hwnd, const wchar_t *text, const wchar_t *title, BOOL show)
{
    HWND hmsg = GetDlgItem(hwnd, ID_TXT_MESSAGE);

    /* Start adding new message at the top */
    SendMessage(hmsg, EM_SETSEL, 0, 0);

    CHARFORMATW cfm = { .cbSize = sizeof(CHARFORMATW)};
    if (title && wcslen(title))
    {
        /* Increase font size and set font color for title of the message */
        SendMessage(hmsg, EM_GETCHARFORMAT, SCF_DEFAULT, (LPARAM) &cfm);
        cfm.dwMask = CFM_SIZE|CFM_COLOR;
        cfm.yHeight = MulDiv(cfm.yHeight, 4, 3); /* scale up by 1.33: 12 pt if default is 9 pt */
        cfm.crTextColor = RGB(0, 0x33, 0x99);
        cfm.dwEffects = 0;

        SendMessage(hmsg, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM) &cfm);
        SendMessage(hmsg, EM_REPLACESEL, FALSE, (LPARAM) title);
        SendMessage(hmsg, EM_REPLACESEL, FALSE, (LPARAM) L"\n");
    }

    /* Revert to default font and set the text */
    SendMessage(hmsg, EM_GETCHARFORMAT, SCF_DEFAULT, (LPARAM) &cfm);
    SendMessage(hmsg, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM) &cfm);
    if (text)
    {
        SendMessage(hmsg, EM_REPLACESEL, FALSE, (LPARAM) text);
        SendMessage(hmsg, EM_REPLACESEL, FALSE, (LPARAM) L"\n");
    }

    /* Select top of the message and scroll to there */
    SendMessage(hmsg, EM_SETSEL, 0, 0);
    SendMessage(hmsg, EM_SCROLLCARET, 0, 0);

    if (show)
    {
        SetForegroundWindow(hwnd);
        ShowWindow(hwnd, SW_SHOW);
    }
}

/* A modeless message box.
 * Use AddMessageBoxText to add content and display
 * the window. On WM_CLOSE the window is hidden, not destroyed.
 */
static INT_PTR CALLBACK
MessageDialogFunc(HWND hwnd, UINT msg, UNUSED WPARAM wParam, LPARAM lParam)
{
    HICON hIcon;
    HWND hmsg;
    const UINT top_margin = DPI_SCALE(16);
    const UINT side_margin = DPI_SCALE(20);

    switch (msg)
    {
    case WM_INITDIALOG:
        hIcon = LoadLocalizedIcon(ID_ICO_APP);
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, (WPARAM) (ICON_SMALL), (LPARAM) (hIcon));
            SendMessage(hwnd, WM_SETICON, (WPARAM) (ICON_BIG), (LPARAM) (hIcon));
        }
        hmsg = GetDlgItem(hwnd, ID_TXT_MESSAGE);
        SetWindowText(hwnd, L"OpenVPN Messages");
        SendMessage(hmsg, EM_SETMARGINS, EC_LEFTMARGIN|EC_RIGHTMARGIN,
                         MAKELPARAM(side_margin, side_margin));

        /* Position the window close to top right corner of the screen */
        RECT rc;
        GetWindowRect(hwnd, &rc);
        OffsetRect(&rc, -rc.left, -rc.top);
        int ox = GetSystemMetrics(SM_CXSCREEN); /* screen size along x */
        ox -= rc.right + DPI_SCALE(rand()%50 + 25);
        int oy = DPI_SCALE(rand()%50 + 25);
        SetWindowPos(hwnd, HWND_TOP, ox > 0 ? ox:0, oy, 0, 0, SWP_NOSIZE);

        return TRUE;

    case WM_SIZE:
        hmsg = GetDlgItem(hwnd, ID_TXT_MESSAGE);
        /* leave some space as top margin */
        SetWindowPos(hmsg, NULL, 0, top_margin, LOWORD(lParam), HIWORD(lParam)-top_margin, 0);
        InvalidateRect(hwnd, NULL, TRUE);
        break;

    /* set the whole client area background to white */
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return (INT_PTR) GetStockObject(WHITE_BRUSH);
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TXT_MESSAGE)
        {
            /* The caret is distracting in a readonly msg box: hide it when we get focus */
            if (HIWORD(wParam) == EN_SETFOCUS)
            {
                HideCaret((HWND)lParam);
            }
            else if (HIWORD(wParam) == EN_KILLFOCUS)
            {
                ShowCaret((HWND)lParam);
            }
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;
    }

    return 0;
}