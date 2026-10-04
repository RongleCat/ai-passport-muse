/*
 * Passport screen copy. Icons stay on Montserrat (LV_SYMBOL_* is not in the
 * CJK subset). Words use muse_font_cjk_14. Logs keep the English strings.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_MUSE_CJK_FONT && CONFIG_MUSE_BOARD_PASSPORT
#define MUSE_UI_ZH 1
#define MUSE_UI_T(en, zh) (zh)
#else
#define MUSE_UI_ZH 0
#define MUSE_UI_T(en, zh) (en)
#endif
