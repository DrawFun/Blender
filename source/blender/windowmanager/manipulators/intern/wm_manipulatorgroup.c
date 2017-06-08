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
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/windowmanager/manipulators/intern/wm_manipulatorgroup.c
 *  \ingroup wm
 *
 * \name Manipulator-Group
 *
 * Manipulator-groups store and manage groups of manipulators. They can be
 * attached to modal handlers and have own keymaps.
 */

#include <stdlib.h>
#include <string.h>

#include "BKE_context.h"
#include "BKE_main.h"
#include "BKE_report.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BPY_extern.h"

#include "DNA_manipulator_types.h"

#include "ED_screen.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"
#include "wm_event_system.h"

/* own includes */
#include "wm_manipulator_wmapi.h"
#include "wm_manipulator_intern.h"


/* -------------------------------------------------------------------- */
/** \name wmManipulatorGroup
 *
 * \{ */

/* wmManipulatorGroup.flag */
enum {
	WM_MANIPULATORGROUP_INITIALIZED = (1 << 2), /* mgroup has been initialized */
};

/**
 * Create a new manipulator-group from \a wgt.
 */
wmManipulatorGroup *wm_manipulatorgroup_new_from_type(
        wmManipulatorMap *mmap, wmManipulatorGroupType *wgt)
{
	wmManipulatorGroup *mgroup = MEM_callocN(sizeof(*mgroup), "manipulator-group");
	mgroup->type = wgt;

	/* keep back-link */
	mgroup->parent_mmap = mmap;

	BLI_addtail(&mmap->manipulator_groups, mgroup);

	return mgroup;
}

void wm_manipulatorgroup_free(bContext *C, wmManipulatorGroup *mgroup)
{
	wmManipulatorMap *mmap = mgroup->parent_mmap;
	for (wmManipulator *manipulator = mgroup->manipulators.first; manipulator;) {
		wmManipulator *manipulator_next = manipulator->next;
		WM_manipulator_free(&mgroup->manipulators, mmap, manipulator, C);
		manipulator = manipulator_next;
	}
	BLI_assert(BLI_listbase_is_empty(&mgroup->manipulators));

#ifdef WITH_PYTHON
	if (mgroup->py_instance) {
		/* do this first in case there are any __del__ functions or
		 * similar that use properties */
		BPY_DECREF_RNA_INVALIDATE(mgroup->py_instance);
	}
#endif

	if (mgroup->reports && (mgroup->reports->flag & RPT_FREE)) {
		BKE_reports_clear(mgroup->reports);
		MEM_freeN(mgroup->reports);
	}

	if (mgroup->customdata_free) {
		mgroup->customdata_free(mgroup->customdata);
	}
	else {
		MEM_SAFE_FREE(mgroup->customdata);
	}

	BLI_remlink(&mmap->manipulator_groups, mgroup);

	MEM_freeN(mgroup);
}

/**
 * Add \a manipulator to \a mgroup and make sure its name is unique within the group.
 */
void wm_manipulatorgroup_manipulator_register(wmManipulatorGroup *mgroup, wmManipulator *manipulator)
{
	BLI_assert(!BLI_findstring(&mgroup->manipulators, manipulator->name, offsetof(wmManipulator, name)));
	BLI_addtail(&mgroup->manipulators, manipulator);
	manipulator->parent_mgroup = mgroup;
}

void wm_manipulatorgroup_attach_to_modal_handler(
        bContext *C, wmEventHandler *handler,
        wmManipulatorGroupType *wgt, wmOperator *op)
{
	/* maybe overly careful, but manipulator-grouptype could come from a failed creation */
	if (!wgt) {
		return;
	}

	/* now instantiate the manipulator-map */
	wgt->op = op;

	/* try to find map in handler region that contains wgt */
	if (handler->op_region && handler->op_region->manipulator_map) {
		handler->manipulator_map = handler->op_region->manipulator_map;
		ED_region_tag_redraw(handler->op_region);
	}

	WM_event_add_mousemove(C);
}

wmManipulator *wm_manipulatorgroup_find_intersected_mainpulator(
        const wmManipulatorGroup *mgroup, bContext *C, const wmEvent *event,
        unsigned char *part)
{
	for (wmManipulator *manipulator = mgroup->manipulators.first; manipulator; manipulator = manipulator->next) {
		if (manipulator->type->intersect && (manipulator->flag & WM_MANIPULATOR_HIDDEN) == 0) {
			if ((*part = manipulator->type->intersect(C, manipulator, event))) {
				return manipulator;
			}
		}
	}

	return NULL;
}

/**
 * Adds all manipulators of \a mgroup that can be selected to the head of \a listbase. Added items need freeing!
 */
void wm_manipulatorgroup_intersectable_manipulators_to_list(const wmManipulatorGroup *mgroup, ListBase *listbase)
{
	for (wmManipulator *manipulator = mgroup->manipulators.first; manipulator; manipulator = manipulator->next) {
		if ((manipulator->flag & WM_MANIPULATOR_HIDDEN) == 0) {
			if (((mgroup->type->flag & WM_MANIPULATORGROUPTYPE_3D) && manipulator->type->draw_select) ||
			    ((mgroup->type->flag & WM_MANIPULATORGROUPTYPE_3D) == 0 && manipulator->type->intersect))
			{
				BLI_addhead(listbase, BLI_genericNodeN(manipulator));
			}
		}
	}
}

void wm_manipulatorgroup_ensure_initialized(wmManipulatorGroup *mgroup, const bContext *C)
{
	/* prepare for first draw */
	if (UNLIKELY((mgroup->flag & WM_MANIPULATORGROUP_INITIALIZED) == 0)) {
		mgroup->type->setup(C, mgroup);

		/* Not ideal, initialize keymap here, needed for RNA runtime generated manipulators. */
		wmManipulatorGroupType *wgt = mgroup->type;
		if (wgt->keymap == NULL) {
			wmWindowManager *wm = CTX_wm_manager(C);
			wm_manipulatorgrouptype_setup_keymap(wgt, wm->defaultconf);
			BLI_assert(wgt->keymap != NULL);
		}

		mgroup->flag |= WM_MANIPULATORGROUP_INITIALIZED;
	}
}

bool wm_manipulatorgroup_is_visible(const wmManipulatorGroup *mgroup, const bContext *C)
{
	/* Check for poll function, if manipulator-group belongs to an operator, also check if the operator is running. */
	return ((mgroup->type->flag & WM_MANIPULATORGROUPTYPE_OP) == 0 || mgroup->type->op) &&
	       (!mgroup->type->poll || mgroup->type->poll(C, mgroup->type));
}

bool wm_manipulatorgroup_is_visible_in_drawstep(const wmManipulatorGroup *mgroup, const int drawstep)
{
	switch (drawstep) {
		case WM_MANIPULATORMAP_DRAWSTEP_2D:
			return (mgroup->type->flag & WM_MANIPULATORGROUPTYPE_3D) == 0;
		case WM_MANIPULATORMAP_DRAWSTEP_3D:
			return (mgroup->type->flag & WM_MANIPULATORGROUPTYPE_3D);
		case WM_MANIPULATORMAP_DRAWSTEP_IN_SCENE:
			return (mgroup->type->flag & WM_MANIPULATORGROUPTYPE_DEPTH_3D);
		default:
			BLI_assert(0);
			return false;
	}
}

/** \name Manipulator operators
 *
 * Basic operators for manipulator interaction with user configurable keymaps.
 *
 * \{ */

static int manipulator_select_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
	ARegion *ar = CTX_wm_region(C);
	wmManipulatorMap *mmap = ar->manipulator_map;
	wmManipulator ***sel = &mmap->mmap_context.selected_manipulator;
	wmManipulator *highlighted = mmap->mmap_context.highlighted_manipulator;

	bool extend = RNA_boolean_get(op->ptr, "extend");
	bool deselect = RNA_boolean_get(op->ptr, "deselect");
	bool toggle = RNA_boolean_get(op->ptr, "toggle");

	/* deselect all first */
	if (extend == false && deselect == false && toggle == false) {
		wm_manipulatormap_deselect_all(mmap, sel);
		BLI_assert(*sel == NULL && mmap->mmap_context.tot_selected == 0);
	}

	if (highlighted) {
		const bool is_selected = (highlighted->state & WM_MANIPULATOR_STATE_SELECT);
		bool redraw = false;

		if (toggle) {
			/* toggle: deselect if already selected, else select */
			deselect = is_selected;
		}

		if (deselect) {
			if (is_selected && wm_manipulator_deselect(mmap, highlighted)) {
				redraw = true;
			}
		}
		else if (wm_manipulator_select(C, mmap, highlighted)) {
			redraw = true;
		}

		if (redraw) {
			ED_region_tag_redraw(ar);
		}

		return OPERATOR_FINISHED;
	}
	else {
		BLI_assert(0);
		return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
	}

	return OPERATOR_PASS_THROUGH;
}

void MANIPULATORGROUP_OT_manipulator_select(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Manipulator Select";
	ot->description = "Select the currently highlighted manipulator";
	ot->idname = "MANIPULATORGROUP_OT_manipulator_select";

	/* api callbacks */
	ot->invoke = manipulator_select_invoke;

	ot->flag = OPTYPE_UNDO;

	WM_operator_properties_mouse_select(ot);
}

typedef struct ManipulatorTweakData {
	wmManipulatorMap *mmap;
	wmManipulator *active;

	int init_event; /* initial event type */
	int flag;       /* tweak flags */
} ManipulatorTweakData;

static void manipulator_tweak_finish(bContext *C, wmOperator *op, const bool cancel)
{
	ManipulatorTweakData *mtweak = op->customdata;
	if (mtweak->active->type->exit) {
		mtweak->active->type->exit(C, mtweak->active, cancel);
	}
	wm_manipulatormap_set_active_manipulator(mtweak->mmap, C, NULL, NULL);
	MEM_freeN(mtweak);
}

static int manipulator_tweak_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
	ManipulatorTweakData *mtweak = op->customdata;
	wmManipulator *manipulator = mtweak->active;

	if (!manipulator) {
		BLI_assert(0);
		return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
	}

	if (event->type == mtweak->init_event && event->val == KM_RELEASE) {
		manipulator_tweak_finish(C, op, false);
		return OPERATOR_FINISHED;
	}


	if (event->type == EVT_MODAL_MAP) {
		switch (event->val) {
			case TWEAK_MODAL_CANCEL:
				manipulator_tweak_finish(C, op, true);
				return OPERATOR_CANCELLED;
			case TWEAK_MODAL_CONFIRM:
				manipulator_tweak_finish(C, op, false);
				return OPERATOR_FINISHED;
			case TWEAK_MODAL_PRECISION_ON:
				mtweak->flag |= WM_MANIPULATOR_TWEAK_PRECISE;
				break;
			case TWEAK_MODAL_PRECISION_OFF:
				mtweak->flag &= ~WM_MANIPULATOR_TWEAK_PRECISE;
				break;
		}
	}

	/* handle manipulator */
	if (manipulator->custom_modal) {
		manipulator->custom_modal(C, manipulator, event, mtweak->flag);
	}
	else if (manipulator->type->modal) {
		manipulator->type->modal(C, manipulator, event, mtweak->flag);
	}

	/* Ugly hack to send manipulator events */
	((wmEvent *)event)->type = EVT_MANIPULATOR_UPDATE;

	/* always return PASS_THROUGH so modal handlers
	 * with manipulators attached can update */
	return OPERATOR_PASS_THROUGH;
}

static int manipulator_tweak_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
	ARegion *ar = CTX_wm_region(C);
	wmManipulatorMap *mmap = ar->manipulator_map;
	wmManipulator *manipulator = mmap->mmap_context.highlighted_manipulator;

	if (!manipulator) {
		/* wm_handlers_do_intern shouldn't let this happen */
		BLI_assert(0);
		return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
	}


	/* activate highlighted manipulator */
	wm_manipulatormap_set_active_manipulator(mmap, C, event, manipulator);

	/* XXX temporary workaround for modal manipulator operator
	 * conflicting with modal operator attached to manipulator */
	if (manipulator->opname) {
		wmOperatorType *ot = WM_operatortype_find(manipulator->opname, true);
		if (ot->modal) {
			return OPERATOR_FINISHED;
		}
	}


	ManipulatorTweakData *mtweak = MEM_mallocN(sizeof(ManipulatorTweakData), __func__);

	mtweak->init_event = event->type;
	mtweak->active = mmap->mmap_context.highlighted_manipulator;
	mtweak->mmap = mmap;
	mtweak->flag = 0;

	op->customdata = mtweak;

	WM_event_add_modal_handler(C, op);

	return OPERATOR_RUNNING_MODAL;
}

void MANIPULATORGROUP_OT_manipulator_tweak(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Manipulator Tweak";
	ot->description = "Tweak the active manipulator";
	ot->idname = "MANIPULATORGROUP_OT_manipulator_tweak";

	/* api callbacks */
	ot->invoke = manipulator_tweak_invoke;
	ot->modal = manipulator_tweak_modal;

	ot->flag = OPTYPE_UNDO;
}

/** \} */ // Manipulator operators


static wmKeyMap *manipulatorgroup_tweak_modal_keymap(wmKeyConfig *keyconf, const char *mgroupname)
{
	wmKeyMap *keymap;
	char name[KMAP_MAX_NAME];

	static EnumPropertyItem modal_items[] = {
		{TWEAK_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
		{TWEAK_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
		{TWEAK_MODAL_PRECISION_ON, "PRECISION_ON", 0, "Enable Precision", ""},
		{TWEAK_MODAL_PRECISION_OFF, "PRECISION_OFF", 0, "Disable Precision", ""},
		{0, NULL, 0, NULL, NULL}
	};


	BLI_snprintf(name, sizeof(name), "%s Tweak Modal Map", mgroupname);
	keymap = WM_modalkeymap_get(keyconf, name);

	/* this function is called for each spacetype, only needs to add map once */
	if (keymap && keymap->modal_items)
		return NULL;

	keymap = WM_modalkeymap_add(keyconf, name, modal_items);


	/* items for modal map */
	WM_modalkeymap_add_item(keymap, ESCKEY, KM_PRESS, KM_ANY, 0, TWEAK_MODAL_CANCEL);
	WM_modalkeymap_add_item(keymap, RIGHTMOUSE, KM_PRESS, KM_ANY, 0, TWEAK_MODAL_CANCEL);

	WM_modalkeymap_add_item(keymap, RETKEY, KM_PRESS, KM_ANY, 0, TWEAK_MODAL_CONFIRM);
	WM_modalkeymap_add_item(keymap, PADENTER, KM_PRESS, KM_ANY, 0, TWEAK_MODAL_CONFIRM);

	WM_modalkeymap_add_item(keymap, RIGHTSHIFTKEY, KM_PRESS, KM_ANY, 0, TWEAK_MODAL_PRECISION_ON);
	WM_modalkeymap_add_item(keymap, RIGHTSHIFTKEY, KM_RELEASE, KM_ANY, 0, TWEAK_MODAL_PRECISION_OFF);
	WM_modalkeymap_add_item(keymap, LEFTSHIFTKEY, KM_PRESS, KM_ANY, 0, TWEAK_MODAL_PRECISION_ON);
	WM_modalkeymap_add_item(keymap, LEFTSHIFTKEY, KM_RELEASE, KM_ANY, 0, TWEAK_MODAL_PRECISION_OFF);


	WM_modalkeymap_assign(keymap, "MANIPULATORGROUP_OT_manipulator_tweak");

	return keymap;
}

/**
 * Common default keymap for manipulator groups
 */
wmKeyMap *WM_manipulatorgroup_keymap_common(const struct wmManipulatorGroupType *wgt, wmKeyConfig *config)
{
	/* Use area and region id since we might have multiple manipulators with the same name in different areas/regions */
	wmKeyMap *km = WM_keymap_find(config, wgt->name, wgt->spaceid, wgt->regionid);

	WM_keymap_add_item(km, "MANIPULATORGROUP_OT_manipulator_tweak", ACTIONMOUSE, KM_PRESS, KM_ANY, 0);
	manipulatorgroup_tweak_modal_keymap(config, wgt->name);

	return km;
}

/**
 * Variation of #WM_manipulatorgroup_keymap_common but with keymap items for selection
 */
wmKeyMap *WM_manipulatorgroup_keymap_common_sel(const struct wmManipulatorGroupType *wgt, wmKeyConfig *config)
{
	/* Use area and region id since we might have multiple manipulators with the same name in different areas/regions */
	wmKeyMap *km = WM_keymap_find(config, wgt->name, wgt->spaceid, wgt->regionid);

	WM_keymap_add_item(km, "MANIPULATORGROUP_OT_manipulator_tweak", ACTIONMOUSE, KM_PRESS, KM_ANY, 0);
	manipulatorgroup_tweak_modal_keymap(config, wgt->name);

	wmKeyMapItem *kmi = WM_keymap_add_item(km, "MANIPULATORGROUP_OT_manipulator_select", SELECTMOUSE, KM_PRESS, 0, 0);
	RNA_boolean_set(kmi->ptr, "extend", false);
	RNA_boolean_set(kmi->ptr, "deselect", false);
	RNA_boolean_set(kmi->ptr, "toggle", false);
	kmi = WM_keymap_add_item(km, "MANIPULATORGROUP_OT_manipulator_select", SELECTMOUSE, KM_PRESS, KM_SHIFT, 0);
	RNA_boolean_set(kmi->ptr, "extend", false);
	RNA_boolean_set(kmi->ptr, "deselect", false);
	RNA_boolean_set(kmi->ptr, "toggle", true);

	return km;
}

/** \} */ /* wmManipulatorGroup */

/* -------------------------------------------------------------------- */
/** \name wmManipulatorGroupType
 *
 * \{ */

struct wmManipulatorGroupType *WM_manipulatorgrouptype_find(
        struct wmManipulatorMapType *mmaptype,
        const char *idname)
{
	/* could use hash lookups as operator types do, for now simple search. */
	for (wmManipulatorGroupType *wgt = mmaptype->manipulator_grouptypes.first;
	     wgt;
	     wgt = wgt->next)
	{
		if (STREQ(idname, wgt->idname)) {
			return wgt;
		}
	}
	return NULL;
}


static wmManipulatorGroupType *wm_manipulatorgrouptype_append__begin(void)
{
	wmManipulatorGroupType *wgt = MEM_callocN(sizeof(wmManipulatorGroupType), "manipulator-group");
	return wgt;
}
static void wm_manipulatorgrouptype_append__end(
        wmManipulatorMapType *mmaptype, wmManipulatorGroupType *wgt)
{
	BLI_assert(wgt->name != NULL);
	BLI_assert(wgt->idname != NULL);

	wgt->spaceid = mmaptype->spaceid;
	wgt->regionid = mmaptype->regionid;
	BLI_strncpy(wgt->mapidname, mmaptype->idname, MAX_NAME);
	/* if not set, use default */
	if (!wgt->setup_keymap) {
		wgt->setup_keymap = WM_manipulatorgroup_keymap_common;
	}

	/* add the type for future created areas of the same type  */
	BLI_addtail(&mmaptype->manipulator_grouptypes, wgt);
}

/**
 * Use this for registering manipulators on startup. For runtime, use #WM_manipulatorgrouptype_append_runtime.
 */
wmManipulatorGroupType *WM_manipulatorgrouptype_append(
        wmManipulatorMapType *mmaptype, void (*wgt_func)(wmManipulatorGroupType *))
{
	wmManipulatorGroupType *wgt = wm_manipulatorgrouptype_append__begin();
	wgt_func(wgt);
	wm_manipulatorgrouptype_append__end(mmaptype, wgt);
	return wgt;
}

wmManipulatorGroupType *WM_manipulatorgrouptype_append_ptr(
        wmManipulatorMapType *mmaptype, void (*wgt_func)(wmManipulatorGroupType *, void *),
        void *userdata)
{
	wmManipulatorGroupType *wgt = wm_manipulatorgrouptype_append__begin();
	wgt_func(wgt, userdata);
	wm_manipulatorgrouptype_append__end(mmaptype, wgt);
	return wgt;
}

/**
 * Use this for registering manipulators on runtime.
 */
wmManipulatorGroupType *WM_manipulatorgrouptype_append_runtime(
        const Main *main, wmManipulatorMapType *mmaptype,
        void (*wgt_func)(wmManipulatorGroupType *))
{
	wmManipulatorGroupType *wgt = WM_manipulatorgrouptype_append(mmaptype, wgt_func);

	/* Main is missing on startup when we create new areas.
	 * So this is only called for manipulators initialized on runtime */
	WM_manipulatorgrouptype_init_runtime(main, mmaptype, wgt);

	return wgt;
}
wmManipulatorGroupType *WM_manipulatorgrouptype_append_ptr_runtime(
        const Main *main, wmManipulatorMapType *mmaptype,
        void (*wgt_func)(wmManipulatorGroupType *, void *),
        void *userdata)
{
	wmManipulatorGroupType *wgt = WM_manipulatorgrouptype_append_ptr(mmaptype, wgt_func, userdata);

	/* Main is missing on startup when we create new areas.
	 * So this is only called for manipulators initialized on runtime */
	WM_manipulatorgrouptype_init_runtime(main, mmaptype, wgt);

	return wgt;
}

void WM_manipulatorgrouptype_init_runtime(
        const Main *bmain, wmManipulatorMapType *mmaptype,
        wmManipulatorGroupType *wgt)
{
	/* init keymap - on startup there's an extra call to init keymaps for 'permanent' manipulator-groups */
	wm_manipulatorgrouptype_setup_keymap(wgt, ((wmWindowManager *)bmain->wm.first)->defaultconf);

	/* now create a manipulator for all existing areas */
	for (bScreen *sc = bmain->screen.first; sc; sc = sc->id.next) {
		for (ScrArea *sa = sc->areabase.first; sa; sa = sa->next) {
			for (SpaceLink *sl = sa->spacedata.first; sl; sl = sl->next) {
				ListBase *lb = (sl == sa->spacedata.first) ? &sa->regionbase : &sl->regionbase;
				for (ARegion *ar = lb->first; ar; ar = ar->next) {
					wmManipulatorMap *mmap = ar->manipulator_map;
					if (mmap && mmap->type == mmaptype) {
						wm_manipulatorgroup_new_from_type(mmap, wgt);

						/* just add here, drawing will occur on next update */
						wm_manipulatormap_set_highlighted_manipulator(mmap, NULL, NULL, 0);
						ED_region_tag_redraw(ar);
					}
				}
			}
		}
	}
}


/**
 * Unlike #WM_manipulatorgrouptype_remove_ptr this doesn't maintain correct state, simply free.
 */
void WM_manipulatorgrouptype_free(wmManipulatorGroupType *wgt)
{
	if (wgt->ext.srna) {
		/* Python operator, allocs own string. */
		MEM_freeN((void *)wgt->idname);
	}

	MEM_freeN(wgt);
}

void WM_manipulatorgrouptype_remove_ptr(bContext *C, Main *bmain, wmManipulatorGroupType *wgt)
{
	for (bScreen *sc = bmain->screen.first; sc; sc = sc->id.next) {
		for (ScrArea *sa = sc->areabase.first; sa; sa = sa->next) {
			for (SpaceLink *sl = sa->spacedata.first; sl; sl = sl->next) {
				ListBase *lb = (sl == sa->spacedata.first) ? &sa->regionbase : &sl->regionbase;
				for (ARegion *ar = lb->first; ar; ar = ar->next) {
					wmManipulatorMap *mmap = ar->manipulator_map;
					if (mmap) {
						wmManipulatorGroup *mgroup, *mgroup_next;
						for (mgroup = mmap->manipulator_groups.first; mgroup; mgroup = mgroup_next) {
							mgroup_next = mgroup->next;
							if (mgroup->type == wgt) {
								BLI_assert(mgroup->parent_mmap == mmap);
								wm_manipulatorgroup_free(C, mgroup);
								ED_region_tag_redraw(ar);
							}
						}
					}
				}
			}
		}
	}

	wmManipulatorMapType *mmaptype = WM_manipulatormaptype_find(&(const struct wmManipulatorMapType_Params) {
	        wgt->mapidname, wgt->spaceid,
	        wgt->regionid});

	BLI_remlink(&mmaptype->manipulator_grouptypes, wgt);
	wgt->prev = wgt->next = NULL;

	WM_manipulatorgrouptype_free(wgt);
}

void wm_manipulatorgrouptype_setup_keymap(wmManipulatorGroupType *wgt, wmKeyConfig *keyconf)
{
	wgt->keymap = wgt->setup_keymap(wgt, keyconf);
}

/** \} */ /* wmManipulatorGroupType */
