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
 * The Original Code is Copyright (C) 2015 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/io/io_cache_library.c
 *  \ingroup editor/io
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLF_translation.h"

#include "BLI_blenlib.h"
#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_utildefines.h"

#include "DNA_cache_library_types.h"
#include "DNA_group_types.h"
#include "DNA_listBase.h"
#include "DNA_modifier_types.h"
#include "DNA_object_force.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"

#include "BKE_anim.h"
#include "BKE_blender.h"
#include "BKE_depsgraph.h"
#include "BKE_cache_library.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_idprop.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "ED_screen.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "WM_api.h"
#include "WM_types.h"

#include "io_cache_library.h"

#include "PTC_api.h"

static int ED_cache_library_active_object_poll(bContext *C)
{
	Object *ob = CTX_data_active_object(C);
	if (!(ob && (ob->transflag & OB_DUPLIGROUP) && ob->dup_group && ob->cache_library))
		return false;
	
	return true;
}

static int ED_cache_modifier_poll(bContext *C)
{
	if (!ED_cache_library_active_object_poll(C))
		return false;
	if (!CTX_data_pointer_get_type(C, "cache_modifier", &RNA_CacheLibraryModifier).data)
		return false;
	
	return true;
}

/********************** new cache library operator *********************/

static int new_cachelib_exec(bContext *C, wmOperator *UNUSED(op))
{
	Object *ob = CTX_data_active_object(C);
	CacheLibrary *cachelib = ob->cache_library;
	Main *bmain = CTX_data_main(C);
	PointerRNA ptr, idptr;
	PropertyRNA *prop;
	
	/* add or copy material */
	if (cachelib) {
		cachelib = BKE_cache_library_copy(cachelib);
	}
	else {
		cachelib = BKE_cache_library_add(bmain, DATA_("CacheLibrary"));
	}
	
	/* enable fake user by default */
	cachelib->id.flag |= LIB_FAKEUSER;
	
	/* hook into UI */
	UI_context_active_but_prop_get_templateID(C, &ptr, &prop);
	
	if (prop) {
		/* when creating new ID blocks, use is already 1, but RNA
		* pointer se also increases user, so this compensates it */
		cachelib->id.us--;
		
		RNA_id_pointer_create(&cachelib->id, &idptr);
		RNA_property_pointer_set(&ptr, prop, idptr);
		RNA_property_update(C, &ptr, prop);
	}
	
	WM_event_add_notifier(C, NC_SCENE, cachelib);
	
	return OPERATOR_FINISHED;
}

void CACHELIBRARY_OT_new(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "New Cache Library";
	ot->idname = "CACHELIBRARY_OT_new";
	ot->description = "Add a new cache library";
	
	/* api callbacks */
	ot->poll = ED_operator_object_active;
	ot->exec = new_cachelib_exec;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/********************** delete cache library operator *********************/

static int cache_library_delete_exec(bContext *C, wmOperator *UNUSED(op))
{
	Main *bmain = CTX_data_main(C);
	Object *ob = CTX_data_active_object(C);
	CacheLibrary *cachelib = ob->cache_library;
	
	BKE_cache_library_unlink(cachelib);
	BKE_libblock_free(bmain, cachelib);
	
	WM_event_add_notifier(C, NC_SCENE, cachelib);
	
	return OPERATOR_FINISHED;
}

void CACHELIBRARY_OT_delete(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Delete Cache Library";
	ot->idname = "CACHELIBRARY_OT_delete";
	ot->description = "Delete a cache library data block";
	
	/* api callbacks */
	ot->exec = cache_library_delete_exec;
	ot->invoke = WM_operator_confirm;
	ot->poll = ED_cache_library_active_object_poll;
	
	/* flags */
	ot->flag = OPTYPE_UNDO;
}

/********************** bake cache operator *********************/

typedef enum eCacheLibraryBake_EvalMode {
	CACHELIBRARY_BAKE_PREVIEW       = (1 << 0), /* evaluate data with preview settings */
	CACHELIBRARY_BAKE_RENDER        = (1 << 1), /* evaluate data with render settings */
} eCacheLibraryBake_EvalMode;

static int cache_library_bake_poll(bContext *C)
{
	Object *ob = CTX_data_active_object(C);
	
	if (!ob || !(ob->transflag & OB_DUPLIGROUP) || !ob->dup_group || !ob->cache_library)
		return false;
	/* disable when the result is not displayed, just to avoid confusing situations */
	if (ob->cache_library->display_mode != CACHE_LIBRARY_DISPLAY_RESULT)
		return false;
	
	return true;
}

typedef struct CacheLibraryBakeJob {
	short *stop, *do_update;
	float *progress;
	
	struct Main *bmain;
	struct Scene *scene;
	struct CacheLibrary *cachelib;
	int lay;
	float mat[4][4];
	struct Group *group;
	
	eCacheLibraryBake_EvalMode eval_mode;
	EvaluationContext eval_ctx;
	
	struct PTCWriterArchive *archive;
	struct PTCWriter *writer;
	
	int start_frame, end_frame;
	int origfra;                            /* original frame to reset scene after export */
	float origframelen;                     /* original frame length to reset scene after export */
} CacheLibraryBakeJob;

static bool cache_library_bake_stop(CacheLibraryBakeJob *data)
{
	return (*data->stop) || G.is_break;
}

static void cache_library_bake_set_progress(CacheLibraryBakeJob *data, float progress)
{
	*data->do_update = 1;
	*data->progress = progress;
}

static void cache_library_bake_set_particle_baking(Main *bmain, bool baking)
{
	/* XXX would be nicer to just loop over scene->base here,
	 * but this does not catch all objects included in dupli groups ...
	 */
	Object *ob;
	for (ob = bmain->object.first; ob; ob = ob->id.next) {
		ParticleSystem *psys;
		
		for (psys = ob->particlesystem.first; psys; psys = psys->next) {
			if (baking)
				psys->pointcache->flag |= PTCACHE_BAKING;
			else
				psys->pointcache->flag &= ~PTCACHE_BAKING;
		}
	}
}

static void cache_library_bake_do(CacheLibraryBakeJob *data, bool use_render)
{
	Scene *scene = data->scene;
	int frame, frame_prev, start_frame, end_frame;
	CacheProcessData process_data;
	
	if (cache_library_bake_stop(data))
		return;
	
	/* === prepare === */
	
	process_data.lay = data->lay;
	copy_m4_m4(process_data.mat, data->mat);
	process_data.dupcache = BKE_dupli_cache_new();
	
	switch (data->cachelib->source_mode) {
		case CACHE_LIBRARY_SOURCE_SCENE:
			data->writer = PTC_writer_dupligroup(data->group->id.name, &data->eval_ctx, scene, data->group, data->cachelib);
			break;
		case CACHE_LIBRARY_SOURCE_CACHE:
			data->writer = PTC_writer_duplicache(data->group->id.name, data->group, process_data.dupcache, data->cachelib->data_types, G.debug & G_DEBUG_SIMDATA);
			break;
	}
	if (!data->writer) {
		BKE_dupli_cache_free(process_data.dupcache);
		return;
	}
	
	data->cachelib->flag |= CACHE_LIBRARY_BAKING;
	
	PTC_writer_init(data->writer, data->archive);
	
	start_frame = data->start_frame;
	end_frame = data->end_frame;
	
	/* === frame loop === */
	
	cache_library_bake_set_progress(data, 0.0f);
	for (frame = frame_prev = start_frame; frame <= end_frame; frame_prev = frame++) {
		
		const bool init_strands = (frame == start_frame);
		
		printf("Bake Cache '%s' | Frame %d\n", data->group->id.name+2, frame);
		
		/* XXX Ugly, but necessary to avoid particle caching of paths when not needed.
		 * This takes a lot of time, but is only needed in the first frame.
		 */
		cache_library_bake_set_particle_baking(data->bmain, !init_strands);
		
		scene->r.cfra = frame;
		BKE_scene_update_group_for_newframe(&data->eval_ctx, data->bmain, scene, data->group, scene->lay);
		
		switch (data->cachelib->source_mode) {
			case CACHE_LIBRARY_SOURCE_SCENE:
				BKE_dupli_cache_from_group(scene, data->group, data->cachelib, process_data.dupcache, &data->eval_ctx, init_strands);
				break;
			case CACHE_LIBRARY_SOURCE_CACHE:
				BKE_cache_read_dupli_cache(data->cachelib, process_data.dupcache, scene, data->group, frame, use_render, false);
				break;
		}
		
		BKE_cache_process_dupli_cache(data->cachelib, &process_data, scene, data->group, frame_prev, frame, true, false, true);
		
		PTC_write_sample(data->writer);
		
		cache_library_bake_set_progress(data, (float)(frame - start_frame + 1) / (float)(end_frame - start_frame + 1));
		if (cache_library_bake_stop(data))
			break;
	}
	
	/* === cleanup === */
	
	if (data->writer) {
		PTC_writer_free(data->writer);
		data->writer = NULL;
	}
	
	data->cachelib->flag &= ~CACHE_LIBRARY_BAKING;
	cache_library_bake_set_particle_baking(data->bmain, false);
	
	BKE_dupli_cache_free(process_data.dupcache);
}

/* Warning! Deletes existing files if possible, operator should show confirm dialog! */
static bool cache_library_bake_ensure_file_target(const char *filename)
{
	if (BLI_exists(filename)) {
		if (BLI_is_dir(filename)) {
			return false;
		}
		else if (BLI_is_file(filename)) {
			if (BLI_file_is_writable(filename)) {
				/* returns 0 on success */
				return (BLI_delete(filename, false, false) == 0);
			}
			else {
				return false;
			}
		}
		else {
			return false;
		}
	}
	return true;
}

static void cache_library_bake_start(void *customdata, short *stop, short *do_update, float *progress)
{
	CacheLibraryBakeJob *data = (CacheLibraryBakeJob *)customdata;
	const bool do_preview = data->eval_mode & CACHELIBRARY_BAKE_PREVIEW;
	const bool do_render = data->eval_mode & CACHELIBRARY_BAKE_RENDER;
	const PTCArchiveResolution archive_res = (do_preview ? PTC_RESOLUTION_PREVIEW : 0) | (do_render ? PTC_RESOLUTION_RENDER : 0);
	Scene *scene = data->scene;
	char filename[FILE_MAX];
	char app_name[MAX_NAME];
	IDProperty *metadata;
	
	data->stop = stop;
	data->do_update = do_update;
	data->progress = progress;
	
	data->origfra = scene->r.cfra;
	data->origframelen = scene->r.framelen;
	scene->r.framelen = 1.0f;
	
	BKE_cache_archive_output_path(data->cachelib, filename, sizeof(filename));
	BLI_snprintf(app_name, sizeof(app_name), "Blender %s", versionstr);
	
	metadata = BKE_cache_library_get_output_metadata(data->cachelib, false);
	
	data->archive = PTC_open_writer_archive(FPS, data->start_frame, filename, archive_res, app_name, data->cachelib->description, NULL, metadata);
	
	if (data->archive) {
		
		G.is_break = false;
		
		if (do_preview) {
			data->eval_ctx.mode = DAG_EVAL_VIEWPORT;
			PTC_writer_archive_use_render(data->archive, false);
			cache_library_bake_do(data, false);
		}
		
		if (do_render) {
			data->eval_ctx.mode = DAG_EVAL_RENDER;
			PTC_writer_archive_use_render(data->archive, true);
			cache_library_bake_do(data, true);
		}
		
	}
	
	*do_update = true;
	*stop = 0;
}

static void cache_library_bake_end(void *customdata)
{
	CacheLibraryBakeJob *data = (CacheLibraryBakeJob *)customdata;
	Scene *scene = data->scene;
	
	G.is_rendering = false;
	BKE_spacedata_draw_locks(false);
	
	if (data->writer)
		PTC_writer_free(data->writer);
	if (data->archive)
		PTC_close_writer_archive(data->archive);
	
	/* reset scene frame */
	scene->r.cfra = data->origfra;
	scene->r.framelen = data->origframelen;
	BKE_scene_update_for_newframe(&data->eval_ctx, data->bmain, scene, scene->lay);
}

static void cache_library_bake_init(CacheLibraryBakeJob *data, bContext *C, wmOperator *op)
{
	Object *ob = CTX_data_active_object(C);
	CacheLibrary *cachelib = ob->cache_library;
	Main *bmain = CTX_data_main(C);
	Scene *scene = CTX_data_scene(C);
	
	/* make sure we can write */
	char filename[FILE_MAX];
	BKE_cache_archive_output_path(cachelib, filename, sizeof(filename));
	cache_library_bake_ensure_file_target(filename);
	
	/* XXX annoying hack: needed to prevent data corruption when changing
	 * scene frame in separate threads
	 */
	G.is_rendering = true;
	
	BKE_spacedata_draw_locks(true);
	
	/* setup data */
	data->bmain = bmain;
	data->scene = scene;
	data->cachelib = cachelib;
	data->lay = ob->lay;
	copy_m4_m4(data->mat, ob->obmat);
	data->group = ob->dup_group;
	
	data->eval_mode = RNA_enum_get(op->ptr, "eval_mode");
	
	if (RNA_struct_property_is_set(op->ptr, "start_frame"))
		data->start_frame = RNA_int_get(op->ptr, "start_frame");
	else
		data->start_frame = scene->r.sfra;
	if (RNA_struct_property_is_set(op->ptr, "end_frame"))
		data->end_frame = RNA_int_get(op->ptr, "end_frame");
	else
		data->end_frame = scene->r.efra;
}

static void cache_library_bake_freejob(void *customdata)
{
	CacheLibraryBakeJob *data= (CacheLibraryBakeJob *)customdata;
	MEM_freeN(data);
}

static int cache_library_bake_exec(bContext *C, wmOperator *op)
{
	const bool use_job = RNA_boolean_get(op->ptr, "use_job");
	
	if (use_job) {
		/* when running through invoke, run as a job */
		CacheLibraryBakeJob *data;
		wmJob *wm_job;
		
		/* XXX set WM_JOB_EXCL_RENDER to prevent conflicts with render jobs,
		 * since we need to set G.is_rendering
		 */
		wm_job = WM_jobs_get(CTX_wm_manager(C), CTX_wm_window(C), CTX_data_scene(C), "Cache Library Bake",
		                     WM_JOB_PROGRESS | WM_JOB_EXCL_RENDER, WM_JOB_TYPE_CACHELIBRARY_BAKE);
		
		/* setup data */
		data = MEM_callocN(sizeof(CacheLibraryBakeJob), "Cache Library Bake Job");
		cache_library_bake_init(data, C, op);
		
		WM_jobs_customdata_set(wm_job, data, cache_library_bake_freejob);
		WM_jobs_timer(wm_job, 0.1, NC_SCENE|ND_FRAME, NC_SCENE|ND_FRAME);
		WM_jobs_callbacks(wm_job, cache_library_bake_start, NULL, NULL, cache_library_bake_end);
		
		WM_jobs_start(CTX_wm_manager(C), wm_job);
		WM_cursor_wait(0);
		
		/* add modal handler for ESC */
		WM_event_add_modal_handler(C, op);
		
		return OPERATOR_RUNNING_MODAL;
	}
	else {
		/* in direct execution mode we run this operator blocking instead of using a job */
		CacheLibraryBakeJob data;
		short stop = false, do_update = false;
		float progress = 0.0f;
		
		cache_library_bake_init(&data, C, op);
		
		cache_library_bake_start(&data, &stop, &do_update, &progress);
		cache_library_bake_end(&data);
		
		return OPERATOR_FINISHED;
	}
}

static int cache_library_bake_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
	Object *ob = CTX_data_active_object(C);
	CacheLibrary *cachelib = ob->cache_library;
	
	char filename[FILE_MAX];
	
	if (!cachelib)
		return OPERATOR_CANCELLED;
	
	/* make sure we run a job when exec is called after confirm popup */
	RNA_boolean_set(op->ptr, "use_job", true);
	
	BKE_cache_archive_output_path(cachelib, filename, sizeof(filename));
	
	if (!BKE_cache_archive_path_test(cachelib, cachelib->output_filepath)) {
		BKE_reportf(op->reports, RPT_ERROR, "Cannot create file path for cache library %200s", cachelib->id.name+2);
		return OPERATOR_CANCELLED;
	}
	
	if (BLI_exists(filename)) {
		if (BLI_is_dir(filename)) {
			BKE_reportf(op->reports, RPT_ERROR, "Cache Library target is a directory: %200s", filename);
			return OPERATOR_CANCELLED;
		}
		else if (BLI_is_file(filename)) {
			if (BLI_file_is_writable(filename)) {
				return WM_operator_confirm_message(C, op, "Overwrite?");
			}
			else {
				BKE_reportf(op->reports, RPT_ERROR, "Cannot overwrite Cache Library target: %200s", filename);
				return OPERATOR_CANCELLED;
			}
			
		}
		else {
			BKE_reportf(op->reports, RPT_ERROR, "Invalid Cache Library target: %200s", filename);
			return OPERATOR_CANCELLED;
		}
	}
	else {
		return cache_library_bake_exec(C, op);
	}
}

/* catch esc */
static int cache_library_bake_modal(bContext *C, wmOperator *UNUSED(op), const wmEvent *event)
{
	/* no running job, remove handler and pass through */
	if (!WM_jobs_test(CTX_wm_manager(C), CTX_data_scene(C), WM_JOB_TYPE_CACHELIBRARY_BAKE))
		return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;
	
	/* running bake */
	switch (event->type) {
		case ESCKEY:
			return OPERATOR_RUNNING_MODAL;
	}
	return OPERATOR_PASS_THROUGH;
}

void CACHELIBRARY_OT_bake(wmOperatorType *ot)
{
	PropertyRNA *prop;
	
	static EnumPropertyItem eval_mode_items[] = {
	    {CACHELIBRARY_BAKE_PREVIEW, "PREVIEW", ICON_RESTRICT_VIEW_OFF, "Preview", "Evaluate data with preview settings"},
	    {CACHELIBRARY_BAKE_RENDER, "RENDER", ICON_RESTRICT_RENDER_OFF, "Render", "Evaluate data with render settings"},
	    {0, NULL, 0, NULL, NULL}
	};
	
	/* identifiers */
	ot->name = "Bake";
	ot->description = "Bake cache library";
	ot->idname = "CACHELIBRARY_OT_bake";
	
	/* api callbacks */
	ot->invoke = cache_library_bake_invoke;
	ot->exec = cache_library_bake_exec;
	ot->modal = cache_library_bake_modal;
	ot->poll = cache_library_bake_poll;
	
	/* flags */
	/* no undo for this operator, cannot restore old cache files anyway */
	ot->flag = OPTYPE_REGISTER;
	
	prop = RNA_def_boolean(ot->srna, "use_job", false, "Use Job", "Run operator as a job");
	/* This is in internal property set by the invoke function.
	 * It allows the exec function to be called from both the confirm popup
	 * as well as a direct exec call for running a blocking operator in background mode.
	 */
	RNA_def_property_flag(prop, PROP_HIDDEN);
	
	prop = RNA_def_enum(ot->srna, "eval_mode", eval_mode_items, CACHELIBRARY_BAKE_RENDER, "Evaluation Mode", "Mode to use when evaluating data");
	RNA_def_property_flag(prop, PROP_ENUM_FLAG);
	
	RNA_def_int(ot->srna, "start_frame", 0, INT_MIN, INT_MAX, "Start Frame", "First frame to be cached", INT_MIN, INT_MAX);
	RNA_def_int(ot->srna, "end_frame", 0, INT_MIN, INT_MAX, "End Frame", "Last frame to be cached", INT_MIN, INT_MAX);
}

/* ========================================================================= */

static void parse_whitespace(const char **s)
{
	const char *c = *s;
	while (*c && isspace(*c))
		++c;
	*s = c;
}

static bool parse_literal(const char **s, const char *lit)
{
	const char *c = *s;
	
	if (lit[0] == '\0')
		return true;
	
	parse_whitespace(&c);
	
	while (*c && *lit) {
		if (*c != *lit)
			return false;
		++c;
		++lit;
	}
	*s = c;
	
	return !(*lit);
}

static bool parse_int(const char **s, int *i)
{
	char *end;
	bool found;
	// does whitespace skipping internally
	*i = (int)strtol(*s, &end, 10);
	found = (end != *s);
	*s = end;
	return found;
}

static void parse_error_log(const char *msg, const char *s, int pos)
{
	printf("%s:\n", msg);
	printf("%s\n", s);
	printf("%*s^\n", pos, "");
}

static bool parse_range(const char **s, int *start, int *end)
{
	const char *full = *s;
	if (!parse_int(s, start)) {
		parse_error_log("Invalid range format, expected int\n", full, *s - full);
		return false;
	}
	if (!parse_literal(s, "-")) {
		parse_error_log("Invalid range format, expected '-'\n", full, *s - full);
		return false;
	}
	if (!parse_int(s, end)) {
		parse_error_log("Invalid range format, expected int\n", full, *s - full);
		return false;
	}
	
	return true;
}

static bool parse_slices(const char *s, ListBase *slices)
{
	CacheSlice *slice;
	int start, end;
	do {
		if (!parse_range(&s, &start, &end))
			return false;
		
		slice = MEM_callocN(sizeof(CacheSlice), "cache slice");
		slice->start = start;
		slice->end = end;
		BLI_addtail(slices, slice);
		
		if (!parse_literal(&s, ","))
			break;
	} while (*s);
	
	return true;
}

static int cache_library_archive_slice_exec(bContext *C, wmOperator *op)
{
	Object *ob = CTX_data_active_object(C);
	CacheLibrary *cachelib = ob->cache_library;
	Scene *scene = CTX_data_scene(C);
	
	char *frames = RNA_string_get_alloc(op->ptr, "frames", NULL, 0);
	ListBase slices = {0};
	
	char input_filepath[FILE_MAX], input_filename[FILE_MAX];
	char output_filepath[FILE_MAX], output_filename[FILE_MAX];
	struct PTCReaderArchive *input_archive;
	struct PTCWriterArchive *output_archive;
	PTCArchiveResolution archive_res;
	CacheArchiveInfo info;
	IDProperty *metadata;
	int start_frame = 0, end_frame = 0;
	
	if (!parse_slices(frames, &slices))
		return OPERATOR_CANCELLED;
	
	RNA_string_get(op->ptr, "input_filepath", input_filepath);
	if (input_filepath[0] == '\0')
		return OPERATOR_CANCELLED;
	RNA_string_get(op->ptr, "output_filepath", output_filepath);
	if (output_filepath[0] == '\0')
		return OPERATOR_CANCELLED;
	
	BKE_cache_archive_path_ex(input_filepath, cachelib->id.lib, NULL, input_filename, sizeof(input_filename));
	BKE_cache_archive_path_ex(output_filepath, cachelib->id.lib, NULL, output_filename, sizeof(output_filename));
	
	/* make sure we can write */
	cache_library_bake_ensure_file_target(output_filename);
	
	input_archive = PTC_open_reader_archive(scene, input_filename);
	if (!input_archive) {
		BKE_reportf(op->reports, RPT_ERROR, "Cannot open cache file at '%s'", input_filepath);
		return OPERATOR_CANCELLED;
	}
	
	archive_res = PTC_reader_archive_get_resolutions(input_archive);
	{
		IDPropertyTemplate val;
		val.i = 0;
		metadata = IDP_New(IDP_GROUP, &val, "cache input metadata");
	}
	PTC_get_archive_info(input_archive, &info, metadata);
	PTC_reader_archive_get_frame_range(input_archive, &start_frame, &end_frame);
	
	output_archive = PTC_open_writer_archive(FPS, start_frame, output_filename, archive_res, info.app_name, info.description, NULL, metadata);
	
	IDP_FreeProperty(metadata);
	MEM_freeN(metadata);
	
	if (!output_archive) {
		BKE_reportf(op->reports, RPT_ERROR, "Cannot write to cache file at '%s'", output_filepath);
		return OPERATOR_CANCELLED;
	}
	
	PTC_archive_slice(input_archive, output_archive, &slices);
	
	PTC_close_reader_archive(input_archive);
	PTC_close_writer_archive(output_archive);
	
	return OPERATOR_FINISHED;
}

static int cache_library_archive_slice_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
	return WM_operator_props_popup_confirm(C, op, event);
}

void CACHELIBRARY_OT_archive_slice(wmOperatorType *ot)
{
	PropertyRNA *prop;
	
	/* identifiers */
	ot->name = "Archive Slice";
	ot->description = "Copy a range of frames to a new cache archive";
	ot->idname = "CACHELIBRARY_OT_archive_slice";
	
	/* api callbacks */
	ot->exec = cache_library_archive_slice_exec;
	ot->invoke = cache_library_archive_slice_invoke;
	ot->poll = ED_cache_library_active_object_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	prop = RNA_def_boolean(ot->srna, "use_job", false, "Use Job", "Run operator as a job");
	/* This is in internal property set by the invoke function.
	 * It allows the exec function to be called from both the confirm popup
	 * as well as a direct exec call for running a blocking operator in background mode.
	 */
	RNA_def_property_flag(prop, PROP_HIDDEN);
	
	prop = RNA_def_string(ot->srna, "input_filepath", NULL, FILE_MAX, "Input File Path", "Path to the source cache archive");
	RNA_def_property_subtype(prop, PROP_FILEPATH);
	prop = RNA_def_string(ot->srna, "output_filepath", NULL, FILE_MAX, "Output File Path", "Path to the target cache archive");
	RNA_def_property_subtype(prop, PROP_FILEPATH);
	RNA_def_string(ot->srna, "frames", NULL, 0, "Frames", "Frame ranges <start>..<end>[, <start>..<end>]");
}

/* ========================================================================= */

#if 0
static void ui_item_nlabel(uiLayout *layout, const char *s, size_t len)
{
	char buf[256];
	
	BLI_strncpy(buf, s, sizeof(buf)-1);
	buf[min_ii(len, sizeof(buf)-1)] = '\0';
	
	uiItemL(layout, buf, ICON_NONE);
}

static void archive_info_labels(uiLayout *layout, const char *info)
{
	const char delim[] = {'\n', '\0'};
	const char *cur = info;
	size_t linelen;
	char *sep, *suf;
	
	linelen = BLI_str_partition(cur, delim, &sep, &suf);
	while (sep) {
		ui_item_nlabel(layout, cur, linelen);
		cur = suf;
		
		linelen = BLI_str_partition(cur, delim, &sep, &suf);
	}
	ui_item_nlabel(layout, cur, linelen);
}

static uiBlock *archive_info_popup_create(bContext *C, ARegion *ar, void *arg)
{
	const char *info = arg;
	uiBlock *block;
	uiLayout *layout;
	
	block = UI_block_begin(C, ar, "_popup", UI_EMBOSS);
	UI_block_flag_disable(block, UI_BLOCK_LOOP);
	UI_block_flag_enable(block, UI_BLOCK_KEEP_OPEN | UI_BLOCK_MOVEMOUSE_QUIT);
	
	layout = UI_block_layout(block, UI_LAYOUT_VERTICAL, UI_LAYOUT_PANEL, 0, 0, UI_UNIT_X * 20, UI_UNIT_Y, 0, UI_style_get());
	
	archive_info_labels(layout, info);
	
	UI_block_bounds_set_centered(block, 0);
	UI_block_direction_set(block, UI_DIR_DOWN);
	
	return block;
}
#endif

static void print_stream(void *UNUSED(userdata), const char *s)
{
	printf("%s", s);
}

static int cache_library_archive_info_exec(bContext *C, wmOperator *op)
{
	Object *ob = CTX_data_active_object(C);
	CacheLibrary *cachelib = ob->cache_library;
	Scene *scene = CTX_data_scene(C);
	
	const bool use_cache_info = RNA_boolean_get(op->ptr, "use_cache_info");
	const bool calc_bytes_size = RNA_boolean_get(op->ptr, "calc_bytes_size");
	const bool use_stdout = RNA_boolean_get(op->ptr, "use_stdout");
	const bool use_popup = RNA_boolean_get(op->ptr, "use_popup");
	const bool use_clipboard = RNA_boolean_get(op->ptr, "use_clipboard");
	
	char filepath[FILE_MAX], filename[FILE_MAX];
	struct PTCReaderArchive *archive;
	
	RNA_string_get(op->ptr, "filepath", filepath);
	if (filepath[0] == '\0')
		return OPERATOR_CANCELLED;
	
	BKE_cache_archive_path_ex(filepath, cachelib->id.lib, NULL, filename, sizeof(filename));
	archive = PTC_open_reader_archive(scene, filename);
	if (!archive) {
		BKE_reportf(op->reports, RPT_ERROR, "Cannot open cache file at '%s'", filepath);
		return OPERATOR_CANCELLED;
	}
	
	if (use_cache_info) {
		if (cachelib->archive_info)
			BKE_cache_archive_info_clear(cachelib->archive_info);
		else
			cachelib->archive_info = BKE_cache_archive_info_new();
		
		BLI_strncpy(cachelib->archive_info->filepath, filename, sizeof(cachelib->archive_info->filepath));
		
		PTC_get_archive_info_nodes(archive, cachelib->archive_info, calc_bytes_size);
	}
	
	if (use_stdout) {
		PTC_get_archive_info_stream(archive, print_stream, NULL);
	}
	
	if (use_popup) {
//		UI_popup_block_invoke(C, archive_info_popup_create, info);
	}
	
	if (use_clipboard) {
//		WM_clipboard_text_set(info, false);
	}
	
	PTC_close_reader_archive(archive);
	
	return OPERATOR_FINISHED;
}

void CACHELIBRARY_OT_archive_info(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Archive Info";
	ot->description = "Get archive details from a cache library archive";
	ot->idname = "CACHELIBRARY_OT_archive_info";
	
	/* api callbacks */
	ot->exec = cache_library_archive_info_exec;
	ot->poll = ED_cache_library_active_object_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	RNA_def_string(ot->srna, "filepath", NULL, FILE_MAX, "File Path", "Path to the cache archive");
	RNA_def_boolean(ot->srna, "use_cache_info", false, "Use Cache Library Info", "Store info in the cache library");
	RNA_def_boolean(ot->srna, "calc_bytes_size", false, "Calculate Size", "Calculate overall size of nodes in bytes (can take a while)");
	RNA_def_boolean(ot->srna, "use_stdout", false, "Use stdout", "Print info in standard output");
	RNA_def_boolean(ot->srna, "use_popup", false, "Show Popup", "Display archive info in a popup");
	RNA_def_boolean(ot->srna, "use_clipboard", false, "Copy to Clipboard", "Copy archive info to the clipboard");
}

/* ------------------------------------------------------------------------- */
/* Cache Modifiers */

static int cache_library_add_modifier_exec(bContext *C, wmOperator *op)
{
	Object *ob = CTX_data_active_object(C);
	CacheLibrary *cachelib = ob->cache_library;
	
	eCacheModifier_Type type = RNA_enum_get(op->ptr, "type");
	if (type == eCacheModifierType_None) {
		return OPERATOR_CANCELLED;
	}
	
	BKE_cache_modifier_add(cachelib, NULL, type);
	
	return OPERATOR_FINISHED;
}

void CACHELIBRARY_OT_add_modifier(struct wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Add Cache Modifier";
	ot->description = "Add a cache modifier";
	ot->idname = "CACHELIBRARY_OT_add_modifier";
	
	/* api callbacks */
	ot->exec = cache_library_add_modifier_exec;
	ot->poll = ED_cache_library_active_object_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
	
	RNA_def_enum(ot->srna, "type", cache_modifier_type_items, eCacheModifierType_None, "Type", "Type of modifier to add");
}

static int cache_library_remove_modifier_exec(bContext *C, wmOperator *UNUSED(op))
{
	PointerRNA md_ptr = CTX_data_pointer_get_type(C, "cache_modifier", &RNA_CacheLibraryModifier);
	CacheModifier *md = md_ptr.data;
	CacheLibrary *cachelib = md_ptr.id.data;
	
	if (!md)
		return OPERATOR_CANCELLED;
	
	BKE_cache_modifier_remove(cachelib, md);
	
	return OPERATOR_FINISHED;
}

void CACHELIBRARY_OT_remove_modifier(struct wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Remove Cache Modifier";
	ot->description = "Remove a cache modifier";
	ot->idname = "CACHELIBRARY_OT_remove_modifier";
	
	/* api callbacks */
	ot->exec = cache_library_remove_modifier_exec;
	ot->poll = ED_cache_modifier_poll;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}