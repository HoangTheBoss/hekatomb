/*
 * Copyright (c) 2026 HOS_COW Project
 *
 * Non-replacing Experimental cowMMC Overlay Management & Setup UI for Nyx
 */

#ifndef _GUI_COW_TOOLS_H_
#define _GUI_COW_TOOLS_H_

#include <bdk.h>
#include <libs/lvgl/lvgl.h>

void gui_cow_set_parent_window(lv_obj_t *manage_win, lv_action_t reopen_fn);
bool gui_cow_format_active_summary(const char *base_path, char *out_buf, u32 out_sz);
lv_res_t create_win_cow_tools(lv_obj_t *btn);

#endif /* _GUI_COW_TOOLS_H_ */
