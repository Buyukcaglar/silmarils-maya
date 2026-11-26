/*
 * host_config.h - the Windows host's names, version and links (decisions D4, D5 in docs/DECISIONS.md).
 * Shared by maya_win32.c and maya64.rc.
 */
#ifndef MAYA_HOST_CONFIG_H
#define MAYA_HOST_CONFIG_H

#define HOST_VERSION_MAJOR 1
#define HOST_VERSION_MINOR 0
#define HOST_VERSION_PATCH 0
#define HOST_VERSION_STR "1.0.0"
#define HOST_VERSION_STRW L"1.0.0"

#define HOST_EXE "maya64.exe"
#define HOST_PRODUCT "Le Fetiche Maya 64bit"            /* ASCII: file and resource names */
#define HOST_CAPTIONW L"Le Fétiche Maya 64bit"     /* shown: the title bar and the About box */
#define HOST_AUTHORW L"Onur BUYUKCAGLAR"
#define HOST_ABOUT_URL L"https://github.com/Buyukcaglar/silmarils-maya"   /* D4: one place to change */
#define HOST_CFG_NAME L"maya64.cfg"
#define HOST_SAVES_NAME L"saves"
#define HOST_APPDATA_NAME L"Le Fetiche Maya 64bit"      /* settings and saves when the exe's folder is read-only */

#endif
