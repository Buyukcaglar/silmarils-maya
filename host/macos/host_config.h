/*
 * host_config.h - the macOS host's names, version and links (decisions D4, D5, D24 in docs/DECISIONS.md).
 * The values match host/win32/host_config.h; the version also goes into Info.plist (CMakeLists.txt).
 */
#ifndef MAYA_HOST_CONFIG_H
#define MAYA_HOST_CONFIG_H

#define HOST_VERSION_STR "1.0.0"

#define HOST_PRODUCT "Le Fetiche Maya 64bit"             /* ASCII: folder and file names */
#define HOST_CAPTION "Le F\xC3\xA9tiche Maya 64bit"      /* UTF-8: shown in the title bar, menus and About box */
#define HOST_AUTHOR "Onur BUYUKCAGLAR"
#define HOST_ABOUT_URL "https://github.com/Buyukcaglar/silmarils-maya"   /* D4 */
#define HOST_CFG_NAME "maya64.cfg"
#define HOST_SAVES_NAME "saves"
#define HOST_SUPPORT_NAME "Le Fetiche Maya 64bit"        /* ~/Library/Application Support/<this> */

#endif
