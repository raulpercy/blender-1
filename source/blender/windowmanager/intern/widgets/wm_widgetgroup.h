/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2014 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Julian Eisel, Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/windowmanager/intern/widgets/wm_widgetgroup.h
 *  \ingroup wm
 */

#ifndef __WM_WIDGETGROUP_H__
#define __WM_WIDGETGROUP_H__

#include "BLI_listbase.h"

#include "DNA_widget_types.h"


#if 0
class wmWidgetGroup
{
public:
	wmWidgetGroup();
};
#endif

void widgetgroup_free(bContext *C, struct wmWidgetMap *wmap, wmWidgetGroup *wgroup);

#endif // __WM_WIDGETGROUP_H__