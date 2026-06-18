/**************************************************************************/
/*  resource_loader.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "resource_loader.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/core_bind.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_importer.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/message_queue.h"
#include "core/object/script_language.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/safe_binary_mutex.h"
#include "core/string/print_string.h"
#include "core/string/translation_server.h"
#include "core/templates/rb_set.h"
#include "core/variant/variant_parser.h"
#include "servers/rendering/rendering_server.h"

#ifdef DEBUG_LOAD_THREADED
#define print_lt(m_text) print_line(m_text)
#else
#define print_lt(m_text)
#endif

Ref<ResourceFormatLoader> ResourceLoader::loader[ResourceLoader::MAX_LOADERS];

int ResourceLoader::loader_count = 0;

#ifdef TOOLS_ENABLED
namespace {

static thread_local bool missing_internal_resource_retry_in_progress = false;
static thread_local Vector<String> lazy_missing_internal_load_stack;
static Mutex lazy_missing_internal_lfs_warned_sources_mutex;
static HashSet<String> lazy_missing_internal_lfs_warned_sources;
static Mutex lazy_missing_internal_in_flight_sources_mutex;
static HashSet<String> lazy_missing_internal_in_flight_sources;
static Mutex lazy_missing_internal_source_cache_mutex;
static HashMap<String, String> lazy_missing_internal_source_cache;
static bool lazy_missing_internal_source_cache_built = false;
static Mutex lazy_missing_internal_replacement_cache_mutex;
static HashMap<String, String> lazy_missing_internal_replacement_cache;

struct LazyMissingInternalLoadScope {
	LazyMissingInternalLoadScope(const String &p_path) {
		lazy_missing_internal_load_stack.push_back(p_path);
	}

	~LazyMissingInternalLoadScope() {
		DEV_ASSERT(!lazy_missing_internal_load_stack.is_empty());
		lazy_missing_internal_load_stack.remove_at(lazy_missing_internal_load_stack.size() - 1);
	}

	String get_referrer_path() const {
		if (lazy_missing_internal_load_stack.size() < 2) {
			return String();
		}
		return lazy_missing_internal_load_stack[lazy_missing_internal_load_stack.size() - 2];
	}
};

struct LazyMissingInternalImportTaskData {
	String source_file;
	String internal_path;
};

static bool _try_lazy_reimport_missing_internal_resource(const String &p_internal_path, const String &p_referrer_path, const String &p_type_hint, String *r_retry_path);

static bool _is_lazy_internal_debug_log_enabled() {
	if (!ProjectSettings::get_singleton()) {
		return false;
	}
	return bool(GLOBAL_GET("editor/import/experimental/lazy_missing_internal_debug_log"));
}

static void _log_lazy_internal_debug(const String &p_message) {
	if (_is_lazy_internal_debug_log_enabled()) {
		print_line(p_message);
	}
}

static bool _is_git_lfs_pointer_file(const String &p_path, String &r_oid, int64_t &r_declared_size) {
	r_oid = String();
	r_declared_size = -1;

	Error err;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	if (f.is_null()) {
		return false;
	}

	// Git LFS pointer files are tiny text manifests.
	if (f->get_length() > 1024) {
		return false;
	}

	const String line1 = f->get_line().strip_edges();
	const String line2 = f->get_line().strip_edges();
	const String line3 = f->get_line().strip_edges();

	if (line1 != "version https://git-lfs.github.com/spec/v1") {
		return false;
	}
	if (!line2.begins_with("oid sha256:")) {
		return false;
	}
	if (!line3.begins_with("size ")) {
		return false;
	}

	const String size_str = line3.trim_prefix("size ");
	if (!size_str.is_valid_int()) {
		return false;
	}

	r_oid = line2.trim_prefix("oid sha256:");
	r_declared_size = size_str.to_int();
	return true;
}

static bool _find_existing_internal_variant_path(const String &p_internal_path, String &r_variant_path) {
	if (FileAccess::exists(p_internal_path)) {
		return false;
	}

	const String imported_dir_path = ProjectSettings::get_singleton()->get_project_data_path().path_join("imported");
	if (!p_internal_path.begins_with(imported_dir_path + "/")) {
		return false;
	}

	if (p_internal_path.ends_with(".ctex")) {
		String base_path = p_internal_path;
		Vector<String> candidate_suffixes;
		if (p_internal_path.ends_with(".s3tc.ctex")) {
			base_path = p_internal_path.trim_suffix(".s3tc.ctex");
			candidate_suffixes = { ".bptc.ctex", ".etc2.ctex", ".astc.ctex", ".ctex" };
		} else if (p_internal_path.ends_with(".bptc.ctex")) {
			base_path = p_internal_path.trim_suffix(".bptc.ctex");
			candidate_suffixes = { ".s3tc.ctex", ".etc2.ctex", ".astc.ctex", ".ctex" };
		} else if (p_internal_path.ends_with(".etc2.ctex")) {
			base_path = p_internal_path.trim_suffix(".etc2.ctex");
			candidate_suffixes = { ".astc.ctex", ".s3tc.ctex", ".bptc.ctex", ".ctex" };
		} else if (p_internal_path.ends_with(".astc.ctex")) {
			base_path = p_internal_path.trim_suffix(".astc.ctex");
			candidate_suffixes = { ".etc2.ctex", ".s3tc.ctex", ".bptc.ctex", ".ctex" };
		}

		for (const String &suffix : candidate_suffixes) {
			const String candidate_path = base_path + suffix;
			if (FileAccess::exists(candidate_path)) {
				r_variant_path = candidate_path;
				return true;
			}
		}
	}

	Ref<DirAccess> dir = DirAccess::open(p_internal_path.get_base_dir());
	if (dir.is_null() || dir->list_dir_begin() != OK) {
		return false;
	}

	const String target_name = p_internal_path.get_file();
	while (true) {
		const String entry = dir->get_next();
		if (entry.is_empty()) {
			break;
		}
		if (dir->current_is_dir()) {
			continue;
		}
		if (entry.nocasecmp_to(target_name) == 0) {
			r_variant_path = p_internal_path.get_base_dir().path_join(entry);
			dir->list_dir_end();
			return true;
		}
	}

	dir->list_dir_end();
	return false;
}

static bool _is_missing_project_imported_path(const String &p_path) {
	if (!ProjectSettings::get_singleton()) {
		return false;
	}
	const String imported_dir_path = ProjectSettings::get_singleton()->get_project_data_path().path_join("imported");
	return p_path.begins_with(imported_dir_path + "/") && !FileAccess::exists(p_path);
}

static bool _is_missing_project_imported_shared_path(const String &p_path) {
	if (!ProjectSettings::get_singleton()) {
		return false;
	}
	const String imported_shared_dir_path = ProjectSettings::get_singleton()->get_project_data_path().path_join("imported/shared");
	return p_path.begins_with(imported_shared_dir_path + "/") && !FileAccess::exists(p_path);
}

static String _extract_lazy_missing_internal_dependency_path(const String &p_dependency) {
	PackedStringArray dependency_parts = p_dependency.split("::");
	for (int i = dependency_parts.size() - 1; i >= 0; i--) {
		if (dependency_parts[i].contains("://")) {
			return dependency_parts[i];
		}
	}
	return p_dependency;
}

static String _get_lazy_missing_internal_artifact_kind(const String &p_path) {
	const String base_name = p_path.get_file().get_basename();
	const int hash_separator = base_name.find_char('-');
	if (hash_separator == -1) {
		return base_name;
	}
	return base_name.substr(0, hash_separator);
}

static int _get_lazy_missing_internal_basename_match_score(const String &p_a, const String &p_b) {
	const String a = p_a.get_file().get_basename().to_lower();
	const String b = p_b.get_file().get_basename().to_lower();
	const int limit = MIN(a.length(), b.length());

	int prefix_len = 0;
	while (prefix_len < limit && a[prefix_len] == b[prefix_len]) {
		prefix_len++;
	}

	return prefix_len;
}

static bool _import_file_references_internal_path(const String &p_import_file_path, const String &p_internal_path, String &r_source_file) {
	const String import_source_candidate = p_import_file_path.trim_suffix(".import");

	Error err;
	Ref<FileAccess> f = FileAccess::open(p_import_file_path, FileAccess::READ, &err);
	if (f.is_null()) {
		return false;
	}

	VariantParser::StreamFile stream;
	stream.f = f;

	String assign;
	Variant value;
	VariantParser::Tag next_tag;

	int lines = 0;
	String error_text;
	bool path_match = false;

	while (true) {
		assign = Variant();
		next_tag.fields.clear();
		next_tag.name = String();

		err = VariantParser::parse_tag_assign_eof(&stream, lines, error_text, next_tag, assign, value, nullptr, true);
		if (err == ERR_FILE_EOF) {
			break;
		}
		if (err != OK) {
			return false;
		}

		if (!assign.is_empty()) {
			if ((assign == "path" || assign.begins_with("path.")) && String(value) == p_internal_path) {
				path_match = true;
			} else if (assign == "source_file") {
				r_source_file = value;
			}
		} else if (next_tag.name != "remap" && next_tag.name != "deps") {
			break;
		}
	}

	if (!path_match) {
		return false;
	}

	String resolved_source = r_source_file;
	if (resolved_source.is_empty() || !FileAccess::exists(resolved_source)) {
		_log_lazy_internal_debug(vformat("[lazy-missing] source_file invalid in %s (source=%s), trying sibling source", p_import_file_path, resolved_source));
		if (FileAccess::exists(import_source_candidate)) {
			resolved_source = import_source_candidate;
			_log_lazy_internal_debug(vformat("[lazy-missing] using sibling source candidate: %s", resolved_source));
		}
	}

	if (resolved_source.is_empty()) {
		return false;
	}

	r_source_file = resolved_source;
	return true;
}

static bool _read_import_file_internal_paths(const String &p_import_file_path, Vector<String> &r_internal_paths, String &r_source_file) {
	r_internal_paths.clear();
	r_source_file = String();

	const String import_source_candidate = p_import_file_path.trim_suffix(".import");

	Error err;
	Ref<FileAccess> f = FileAccess::open(p_import_file_path, FileAccess::READ, &err);
	if (f.is_null()) {
		return false;
	}

	VariantParser::StreamFile stream;
	stream.f = f;

	String assign;
	Variant value;
	VariantParser::Tag next_tag;

	int lines = 0;
	String error_text;

	while (true) {
		assign = Variant();
		next_tag.fields.clear();
		next_tag.name = String();

		err = VariantParser::parse_tag_assign_eof(&stream, lines, error_text, next_tag, assign, value, nullptr, true);
		if (err == ERR_FILE_EOF) {
			break;
		}
		if (err != OK) {
			return false;
		}

		if (!assign.is_empty()) {
			if (assign == "path" || assign.begins_with("path.")) {
				r_internal_paths.push_back(String(value));
			} else if (assign == "source_file") {
				r_source_file = value;
			}
		} else if (next_tag.name != "remap" && next_tag.name != "deps") {
			break;
		}
	}

	if (r_source_file.is_empty() || !FileAccess::exists(r_source_file)) {
		if (FileAccess::exists(import_source_candidate)) {
			r_source_file = import_source_candidate;
		}
	}

	return !r_internal_paths.is_empty() && !r_source_file.is_empty();
}

static void _register_import_file_internal_paths(const String &p_import_file_path) {
	Vector<String> internal_paths;
	String source_file;
	if (!_read_import_file_internal_paths(p_import_file_path, internal_paths, source_file)) {
		return;
	}

	MutexLock lock(lazy_missing_internal_source_cache_mutex);
	for (const String &internal_path : internal_paths) {
		if (!lazy_missing_internal_source_cache.has(internal_path)) {
			lazy_missing_internal_source_cache.insert(internal_path, source_file);
		}
	}
}

static void _build_lazy_missing_internal_source_cache_recursive(const String &p_dir_path) {
	Ref<DirAccess> dir = DirAccess::open(p_dir_path);
	if (dir.is_null() || dir->list_dir_begin() != OK) {
		return;
	}

	const String project_data_path = ProjectSettings::get_singleton()->get_project_data_path();
	while (true) {
		const String entry = dir->get_next();
		if (entry.is_empty()) {
			break;
		}
		if (entry == "." || entry == "..") {
			continue;
		}

		const String entry_path = p_dir_path.path_join(entry);
		if (dir->current_is_dir()) {
			if (entry_path == project_data_path || entry_path.begins_with(project_data_path + "/")) {
				continue;
			}
			_build_lazy_missing_internal_source_cache_recursive(entry_path);
			continue;
		}

		if (!entry.ends_with(".import")) {
			continue;
		}

		_register_import_file_internal_paths(entry_path);
	}

	dir->list_dir_end();
}

static void _ensure_lazy_missing_internal_source_cache() {
	{
		MutexLock lock(lazy_missing_internal_source_cache_mutex);
		if (lazy_missing_internal_source_cache_built) {
			return;
		}
	}

	_build_lazy_missing_internal_source_cache_recursive("res://");

	MutexLock lock(lazy_missing_internal_source_cache_mutex);
	lazy_missing_internal_source_cache_built = true;
}

static void _lazy_missing_internal_import_task(void *p_userdata) {
	LazyMissingInternalImportTaskData *task = static_cast<LazyMissingInternalImportTaskData *>(p_userdata);
	const String source_file = task->source_file;
	const String internal_path = task->internal_path;
	memdelete(task);

	_log_lazy_internal_debug(vformat("[lazy-missing] background reimporting source=%s for missing internal=%s", source_file, internal_path));
	const bool retry_backup = missing_internal_resource_retry_in_progress;
	missing_internal_resource_retry_in_progress = true;
	const Error import_err = ResourceLoader::import(source_file);
	missing_internal_resource_retry_in_progress = retry_backup;
	if (import_err != OK) {
		_log_lazy_internal_debug(vformat("[lazy-missing] background reimport failed (%d): source=%s internal=%s", int(import_err), source_file, internal_path));
	} else {
		_log_lazy_internal_debug(vformat("[lazy-missing] background reimport succeeded: source=%s internal=%s", source_file, internal_path));
	}
	{
		MutexLock lock(lazy_missing_internal_in_flight_sources_mutex);
		lazy_missing_internal_in_flight_sources.erase(source_file);
	}
}

static bool _find_source_for_internal_resource_recursive(const String &p_dir_path, const String &p_internal_path, String &r_source_file) {
	Ref<DirAccess> dir = DirAccess::open(p_dir_path);
	if (dir.is_null() || dir->list_dir_begin() != OK) {
		return false;
	}

	const String project_data_path = ProjectSettings::get_singleton()->get_project_data_path();
	bool found_match = false;
	while (true) {
		const String entry = dir->get_next();
		if (entry.is_empty()) {
			break;
		}
		if (entry == "." || entry == "..") {
			continue;
		}

		const String entry_path = p_dir_path.path_join(entry);
		if (dir->current_is_dir()) {
			if (entry_path == project_data_path || entry_path.begins_with(project_data_path + "/")) {
				continue;
			}
			if (_find_source_for_internal_resource_recursive(entry_path, p_internal_path, r_source_file)) {
				found_match = true;
				break;
			}
			continue;
		}

		if (!entry.ends_with(".import")) {
			continue;
		}

		String source_file;
		if (_import_file_references_internal_path(entry_path, p_internal_path, source_file)) {
			r_source_file = source_file;
			found_match = true;
			break;
		}
	}

	dir->list_dir_end();
	return found_match;
}

static bool _find_cached_lazy_missing_internal_replacement_path(const String &p_internal_path, String &r_replacement_path) {
	MutexLock lock(lazy_missing_internal_replacement_cache_mutex);
	const String *replacement_path = lazy_missing_internal_replacement_cache.getptr(p_internal_path);
	if (replacement_path == nullptr) {
		return false;
	}
	if (!FileAccess::exists(*replacement_path)) {
		lazy_missing_internal_replacement_cache.erase(p_internal_path);
		return false;
	}
	r_replacement_path = *replacement_path;
	return true;
}

static bool _find_replacement_internal_path_from_source(const String &p_internal_path, const String &p_source_file, const String &p_type_hint, String &r_replacement_path) {
	const String import_file_path = p_source_file + ".import";
	Vector<String> internal_paths;
	String source_file;
	if (!_read_import_file_internal_paths(import_file_path, internal_paths, source_file)) {
		return false;
	}

	const String missing_base_dir = p_internal_path.get_base_dir();
	const String missing_extension = p_internal_path.get_extension().to_lower();
	const String missing_artifact_kind = _get_lazy_missing_internal_artifact_kind(p_internal_path);

	for (const String &internal_path : internal_paths) {
		if (!FileAccess::exists(internal_path)) {
			continue;
		}

		List<String> dependencies;
		ResourceLoader::get_dependencies(internal_path, &dependencies, false);
		for (const String &dependency : dependencies) {
			const String dependency_path = _extract_lazy_missing_internal_dependency_path(dependency);
			if (!dependency_path.begins_with(missing_base_dir + "/")) {
				continue;
			}
			if (!FileAccess::exists(dependency_path)) {
				continue;
			}
			if (dependency_path.get_extension().to_lower() != missing_extension) {
				continue;
			}
			if (_get_lazy_missing_internal_artifact_kind(dependency_path) != missing_artifact_kind) {
				continue;
			}
			if (!p_type_hint.is_empty()) {
				const String dependency_type = ResourceLoader::get_resource_type(dependency_path);
				if (!dependency_type.is_empty() && dependency_type != p_type_hint) {
					continue;
				}
			}

			r_replacement_path = dependency_path;
			{
				MutexLock lock(lazy_missing_internal_replacement_cache_mutex);
				lazy_missing_internal_replacement_cache.insert(p_internal_path, r_replacement_path);
			}
			_log_lazy_internal_debug(vformat("[lazy-missing] using replacement internal path: missing=%s replacement=%s source=%s", p_internal_path, r_replacement_path, p_source_file));
			return true;
		}
	}

	return false;
}

static void _heal_lazy_missing_internal_referrer_dependency(const String &p_referrer_path, const String &p_missing_internal_path, const String &p_replacement_path) {
	if (p_referrer_path.is_empty() || p_replacement_path.is_empty() || p_referrer_path == p_replacement_path) {
		return;
	}
	if (!FileAccess::exists(p_referrer_path)) {
		return;
	}

	const String project_data_path = ProjectSettings::get_singleton()->get_project_data_path();
	if (p_referrer_path == project_data_path || p_referrer_path.begins_with(project_data_path + "/")) {
		return;
	}

	HashMap<String, String> dependency_renames;
	dependency_renames.insert(p_missing_internal_path, p_replacement_path);
	const Error rename_err = ResourceLoader::rename_dependencies(p_referrer_path, dependency_renames);
	if (rename_err == OK) {
		_log_lazy_internal_debug(vformat("[lazy-missing] healed referrer dependency: referrer=%s old=%s new=%s", p_referrer_path, p_missing_internal_path, p_replacement_path));
	} else {
		_log_lazy_internal_debug(vformat("[lazy-missing] failed to heal referrer dependency (%d): referrer=%s old=%s new=%s", int(rename_err), p_referrer_path, p_missing_internal_path, p_replacement_path));
	}
}

static bool _find_scene_import_source_near_referrer(const String &p_referrer_path, String &r_source_file) {
	r_source_file = String();

	if (p_referrer_path.is_empty()) {
		return false;
	}

	Ref<DirAccess> dir = DirAccess::open(p_referrer_path.get_base_dir());
	if (dir.is_null() || dir->list_dir_begin() != OK) {
		return false;
	}

	int best_score = -1;
	String best_source_file;

	while (true) {
		const String entry = dir->get_next();
		if (entry.is_empty()) {
			break;
		}
		if (entry == "." || entry == ".." || dir->current_is_dir() || !entry.ends_with(".import")) {
			continue;
		}

		const String import_file_path = p_referrer_path.get_base_dir().path_join(entry);
		Vector<String> internal_paths;
		String source_file;
		if (!_read_import_file_internal_paths(import_file_path, internal_paths, source_file)) {
			continue;
		}

		Ref<ResourceImporter> importer = ResourceFormatImporter::get_singleton()->get_importer_by_file(source_file);
		if (importer.is_null() || importer->get_resource_type() != "PackedScene") {
			continue;
		}

		const int score = _get_lazy_missing_internal_basename_match_score(p_referrer_path, source_file);
		if (score > best_score) {
			best_score = score;
			best_source_file = source_file;
		}
	}

	dir->list_dir_end();

	if (best_source_file.is_empty()) {
		return false;
	}

	r_source_file = best_source_file;
	_log_lazy_internal_debug(vformat("[lazy-missing] using nearby scene import source: referrer=%s source=%s", p_referrer_path, r_source_file));
	return true;
}

static bool _try_lazy_reimport_missing_internal_resource(const String &p_internal_path, const String &p_referrer_path, const String &p_type_hint, String *r_retry_path) {
	if (r_retry_path != nullptr) {
		*r_retry_path = p_internal_path;
	}

	if (!Engine::get_singleton()->is_editor_hint()) {
		_log_lazy_internal_debug(vformat("[lazy-missing] skip (not editor hint): %s", p_internal_path));
		return false;
	}
	if (!bool(GLOBAL_GET("editor/import/lazy_reimport_on_load"))) {
		_log_lazy_internal_debug(vformat("[lazy-missing] skip (lazy_reimport_on_load disabled): %s", p_internal_path));
		return false;
	}
	if (ResourceLoader::import == nullptr) {
		_log_lazy_internal_debug(vformat("[lazy-missing] skip (ResourceLoader::import is null): %s", p_internal_path));
		return false;
	}
	if (missing_internal_resource_retry_in_progress) {
		_log_lazy_internal_debug(vformat("[lazy-missing] skip (retry already in progress): %s", p_internal_path));
		return false;
	}

	const String imported_dir_path = ProjectSettings::get_singleton()->get_project_data_path().path_join("imported");
	if (!p_internal_path.begins_with(imported_dir_path + "/")) {
		_log_lazy_internal_debug(vformat("[lazy-missing] skip (not project imported path): %s", p_internal_path));
		return false;
	}

	String source_file;
	_ensure_lazy_missing_internal_source_cache();
	{
		MutexLock lock(lazy_missing_internal_source_cache_mutex);
		if (const String *cached_source = lazy_missing_internal_source_cache.getptr(p_internal_path)) {
			source_file = *cached_source;
		}
	}

	if (source_file.is_empty() && _find_source_for_internal_resource_recursive("res://", p_internal_path, source_file)) {
		MutexLock lock(lazy_missing_internal_source_cache_mutex);
		lazy_missing_internal_source_cache.insert(p_internal_path, source_file);
	}

	if (source_file.is_empty() && _is_missing_project_imported_shared_path(p_internal_path) && _find_scene_import_source_near_referrer(p_referrer_path, source_file)) {
		MutexLock lock(lazy_missing_internal_source_cache_mutex);
		lazy_missing_internal_source_cache.insert(p_internal_path, source_file);
	}

	if (source_file.is_empty()) {
		_log_lazy_internal_debug(vformat("[lazy-missing] source not found for internal path: %s", p_internal_path));
		return false;
	}

	if (source_file.is_empty() || source_file == p_internal_path) {
		_log_lazy_internal_debug(vformat("[lazy-missing] resolved source invalid: internal=%s source=%s", p_internal_path, source_file));
		return false;
	}

	String lfs_oid;
	int64_t lfs_declared_size = -1;
	if (_is_git_lfs_pointer_file(source_file, lfs_oid, lfs_declared_size)) {
		{
			MutexLock lock(lazy_missing_internal_lfs_warned_sources_mutex);
			if (!lazy_missing_internal_lfs_warned_sources.has(source_file)) {
				lazy_missing_internal_lfs_warned_sources.insert(source_file);
				WARN_PRINT(vformat("Lazy missing-internal recovery skipped for %s: source asset is a Git LFS pointer without payload (%s, oid=%s, expected_size=%s). Pull LFS content for this asset to regenerate the imported texture.",
						p_internal_path, source_file, lfs_oid, String::num_int64(lfs_declared_size)));
			}
		}
		_log_lazy_internal_debug(vformat("[lazy-missing] source is Git LFS pointer (payload unavailable): source=%s oid=%s size=%s internal=%s", source_file, lfs_oid, String::num_int64(lfs_declared_size), p_internal_path));
		return false;
	}

	if (!Thread::is_main_thread()) {
		bool queued = false;
		{
			MutexLock lock(lazy_missing_internal_in_flight_sources_mutex);
			if (!lazy_missing_internal_in_flight_sources.has(source_file)) {
				lazy_missing_internal_in_flight_sources.insert(source_file);
				queued = true;
			}
		}
		if (queued) {
			LazyMissingInternalImportTaskData *task = memnew(LazyMissingInternalImportTaskData);
			task->source_file = source_file;
			task->internal_path = p_internal_path;
			WorkerThreadPool::get_singleton()->add_native_task(_lazy_missing_internal_import_task, task, false, "lazy_missing_internal_reimport");
			_log_lazy_internal_debug(vformat("[lazy-missing] queued background reimport task: source=%s internal=%s", source_file, p_internal_path));
		} else {
			_log_lazy_internal_debug(vformat("[lazy-missing] background reimport already queued: source=%s internal=%s", source_file, p_internal_path));
		}
		return false;
	}

	_log_lazy_internal_debug(vformat("[lazy-missing] reimporting source=%s for missing internal=%s", source_file, p_internal_path));
	missing_internal_resource_retry_in_progress = true;
	const Error import_err = ResourceLoader::import(source_file);
	missing_internal_resource_retry_in_progress = false;
	if (import_err != OK) {
		_log_lazy_internal_debug(vformat("[lazy-missing] reimport failed (%d): source=%s internal=%s", int(import_err), source_file, p_internal_path));
	} else {
		_log_lazy_internal_debug(vformat("[lazy-missing] reimport succeeded: source=%s internal=%s", source_file, p_internal_path));
	}
	if (import_err != OK) {
		return false;
	}

	if (FileAccess::exists(p_internal_path)) {
		return true;
	}

	String replacement_path;
	if (_is_missing_project_imported_shared_path(p_internal_path) && _find_replacement_internal_path_from_source(p_internal_path, source_file, p_type_hint, replacement_path)) {
		_heal_lazy_missing_internal_referrer_dependency(p_referrer_path, p_internal_path, replacement_path);
		if (r_retry_path != nullptr) {
			*r_retry_path = replacement_path;
		}
		return true;
	}

	_log_lazy_internal_debug(vformat("[lazy-missing] reimport finished but requested internal path is still missing: %s", p_internal_path));
	return false;
}

} // namespace
#endif // TOOLS_ENABLED

bool ResourceFormatLoader::recognize_path(const String &p_path, const String &p_for_type) const {
	bool ret = false;
	if (GDVIRTUAL_CALL(_recognize_path, p_path, p_for_type, ret)) {
		return ret;
	}

	List<String> extensions;
	if (p_for_type.is_empty()) {
		get_recognized_extensions(&extensions);
	} else {
		get_recognized_extensions_for_type(p_for_type, &extensions);
	}

	for (const String &E : extensions) {
		const String ext = !E.begins_with(".") ? "." + E : E;
		if (p_path.right(ext.length()).nocasecmp_to(ext) == 0) {
			return true;
		}
	}

	return false;
}

bool ResourceFormatLoader::handles_type(const String &p_type) const {
	bool success = false;
	GDVIRTUAL_CALL(_handles_type, p_type, success);
	return success;
}

void ResourceFormatLoader::get_classes_used(const String &p_path, HashSet<StringName> *r_classes) {
	Vector<String> ret;
	if (GDVIRTUAL_CALL(_get_classes_used, p_path, ret)) {
		for (int i = 0; i < ret.size(); i++) {
			r_classes->insert(ret[i]);
		}
		return;
	}

	String res = get_resource_type(p_path);
	if (!res.is_empty()) {
		r_classes->insert(res);
	}
}

String ResourceFormatLoader::get_resource_type(const String &p_path) const {
	String ret;
	GDVIRTUAL_CALL(_get_resource_type, p_path, ret);
	return ret;
}

String ResourceFormatLoader::get_resource_script_class(const String &p_path) const {
	String ret;
	GDVIRTUAL_CALL(_get_resource_script_class, p_path, ret);
	return ret;
}

ResourceUID::ID ResourceFormatLoader::get_resource_uid(const String &p_path) const {
	int64_t uid = ResourceUID::INVALID_ID;
	if (has_custom_uid_support()) {
		GDVIRTUAL_CALL(_get_resource_uid, p_path, uid);
	} else {
		Ref<FileAccess> file = FileAccess::open(p_path + ".uid", FileAccess::READ);
		if (file.is_valid()) {
			uid = ResourceUID::get_singleton()->text_to_id(file->get_line());
		}
	}
	return uid;
}

bool ResourceFormatLoader::has_custom_uid_support() const {
	return GDVIRTUAL_IS_OVERRIDDEN(_get_resource_uid);
}

void ResourceFormatLoader::get_recognized_extensions_for_type(const String &p_type, List<String> *p_extensions) const {
	if (p_type.is_empty() || handles_type(p_type)) {
		get_recognized_extensions(p_extensions);
	}
}

void ResourceLoader::get_recognized_extensions_for_type(const String &p_type, List<String> *p_extensions) {
	for (int i = 0; i < loader_count; i++) {
		loader[i]->get_recognized_extensions_for_type(p_type, p_extensions);
	}
}

bool ResourceFormatLoader::exists(const String &p_path) const {
	bool success = false;
	if (GDVIRTUAL_CALL(_exists, p_path, success)) {
		return success;
	}
	return FileAccess::exists(p_path); // By default just check file.
}

void ResourceFormatLoader::get_recognized_extensions(List<String> *p_extensions) const {
	PackedStringArray exts;
	if (GDVIRTUAL_CALL(_get_recognized_extensions, exts)) {
		const String *r = exts.ptr();
		for (int i = 0; i < exts.size(); ++i) {
			p_extensions->push_back(r[i]);
		}
	}
}

Ref<Resource> ResourceFormatLoader::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Variant res;
	if (GDVIRTUAL_CALL(_load, p_path, p_original_path, p_use_sub_threads, p_cache_mode, res)) {
		if (res.get_type() == Variant::INT) { // Error code, abort.
			if (r_error) {
				*r_error = (Error)res.operator int64_t();
			}
			return Ref<Resource>();
		} else { // Success, pass on result.
			if (r_error) {
				*r_error = OK;
			}
			return res;
		}
	}

	return Ref<Resource>();
}

void ResourceFormatLoader::get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types) {
	PackedStringArray deps;
	if (GDVIRTUAL_CALL(_get_dependencies, p_path, p_add_types, deps)) {
		const String *r = deps.ptr();
		for (int i = 0; i < deps.size(); ++i) {
			p_dependencies->push_back(r[i]);
		}
	}
}

Error ResourceFormatLoader::rename_dependencies(const String &p_path, const HashMap<String, String> &p_map) {
	Dictionary deps_dict;
	for (KeyValue<String, String> E : p_map) {
		deps_dict[E.key] = E.value;
	}

	Error err = OK;
	GDVIRTUAL_CALL(_rename_dependencies, p_path, deps_dict, err);
	return err;
}

void ResourceFormatLoader::_bind_methods() {
	BIND_ENUM_CONSTANT(CACHE_MODE_IGNORE);
	BIND_ENUM_CONSTANT(CACHE_MODE_REUSE);
	BIND_ENUM_CONSTANT(CACHE_MODE_REPLACE);
	BIND_ENUM_CONSTANT(CACHE_MODE_IGNORE_DEEP);
	BIND_ENUM_CONSTANT(CACHE_MODE_REPLACE_DEEP);

	GDVIRTUAL_BIND(_get_recognized_extensions);
	GDVIRTUAL_BIND(_recognize_path, "path", "type");
	GDVIRTUAL_BIND(_handles_type, "type");
	GDVIRTUAL_BIND(_get_resource_type, "path");
	GDVIRTUAL_BIND(_get_resource_script_class, "path");
	GDVIRTUAL_BIND(_get_resource_uid, "path");
	GDVIRTUAL_BIND(_get_dependencies, "path", "add_types");
	GDVIRTUAL_BIND(_rename_dependencies, "path", "renames");
	GDVIRTUAL_BIND(_exists, "path");
	GDVIRTUAL_BIND(_get_classes_used, "path");
	GDVIRTUAL_BIND(_load, "path", "original_path", "use_sub_threads", "cache_mode");
}

// This should be robust enough to be called redundantly without issues.
void ResourceLoader::LoadToken::clear() {
	WorkerThreadPool::TaskID task_to_await = 0;

	{
		MutexLock thread_load_lock(thread_load_mutex);
		// User-facing tokens shouldn't be deleted until completely claimed.
		DEV_ASSERT(user_rc == 0 && user_path.is_empty());

		if (!local_path.is_empty()) {
			if (task_if_unregistered) {
				memdelete(task_if_unregistered);
				task_if_unregistered = nullptr;
			} else {
				DEV_ASSERT(thread_load_tasks.has(local_path));
				ThreadLoadTask &load_task = thread_load_tasks[local_path];
				if (load_task.task_id && !load_task.awaited) {
					task_to_await = load_task.task_id;
				}
				// Removing a task which is still in progress would be catastrophic.
				// Tokens must be alive until the task thread function is done.
				DEV_ASSERT(load_task.status == THREAD_LOAD_FAILED || load_task.status == THREAD_LOAD_LOADED);
				thread_load_tasks.erase(local_path);
			}
			local_path.clear(); // Mark as already cleared.
			if (task_to_await) {
				for (KeyValue<String, ResourceLoader::ThreadLoadTask> &E : thread_load_tasks) {
					if (E.value.task_id == task_to_await) {
						task_to_await = 0;
						break; // Same task is reused by nested loads, do not wait for completion here.
					}
				}
			}
		}
	}

	// If task is unused, await it here, locally, now the token data is consistent.
	if (task_to_await) {
		int load_nesting_backup = load_nesting;
		load_nesting = 0;
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_to_await);
		DEV_ASSERT(load_nesting == 0);
		load_nesting = load_nesting_backup;
	}
}

ResourceLoader::LoadToken::~LoadToken() {
	clear();
}

Ref<Resource> ResourceLoader::_load(const String &p_path, const String &p_original_path, const String &p_type_hint, CacheMode p_cache_mode, Error *r_error, bool p_use_sub_threads, float *r_progress) {
	const String &original_path = p_original_path.is_empty() ? p_path : p_original_path;
	load_nesting++;
	LazyMissingInternalLoadScope lazy_missing_internal_load_scope(p_path);

	print_verbose(vformat("Loading resource: %s remapped: %s", p_path, _path_remap(p_path)));

#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint() && _is_missing_project_imported_path(p_path)) {
		String existing_variant_path;
		if (_find_existing_internal_variant_path(p_path, existing_variant_path)) {
			_log_lazy_internal_debug(vformat("[lazy-missing] trying existing fallback variant: missing=%s fallback=%s", p_path, existing_variant_path));
			res_ref_overrides.erase(load_nesting);
			load_nesting--;
			return _load(existing_variant_path, p_original_path, p_type_hint, p_cache_mode, r_error, p_use_sub_threads, r_progress);
		}

		String replacement_path;
		if (_find_cached_lazy_missing_internal_replacement_path(p_path, replacement_path)) {
			_log_lazy_internal_debug(vformat("[lazy-missing] trying cached replacement path: missing=%s replacement=%s", p_path, replacement_path));
			res_ref_overrides.erase(load_nesting);
			load_nesting--;
			return _load(replacement_path, p_original_path, p_type_hint, p_cache_mode, r_error, p_use_sub_threads, r_progress);
		}

		String retry_path = p_path;
		if (_try_lazy_reimport_missing_internal_resource(p_path, lazy_missing_internal_load_scope.get_referrer_path(), p_type_hint, &retry_path)) {
			_log_lazy_internal_debug(vformat("[lazy-missing] retrying path after reimport: missing=%s retry=%s", p_path, retry_path));
			res_ref_overrides.erase(load_nesting);
			load_nesting--;
			return _load(retry_path, p_original_path, p_type_hint, p_cache_mode, r_error, p_use_sub_threads, r_progress);
		}

		if (r_error) {
			*r_error = ERR_FILE_NOT_FOUND;
		}
		res_ref_overrides.erase(load_nesting);
		load_nesting--;
		return Ref<Resource>();
	}
#endif

	// Try all loaders and pick the first match for the type hint
	bool found = false;
	Ref<Resource> res;
	for (int i = 0; i < loader_count; i++) {
		if (!loader[i]->recognize_path(p_path, p_type_hint)) {
			continue;
		}
		found = true;
		res = loader[i]->load(p_path, original_path, r_error, p_use_sub_threads, r_progress, p_cache_mode);
		if (res.is_valid()) {
			break;
		}
	}

	res_ref_overrides.erase(load_nesting);
	load_nesting--;

	if (res.is_valid()) {
		return res;
	} else {
		print_verbose(vformat("Failed loading resource: %s", p_path));
	}

#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		if (ResourceFormatImporter::get_singleton()->get_importer_by_file(p_path).is_valid()) {
			// The format is known to the editor, but the file hasn't been imported
			// (otherwise, ResourceFormatImporter would have been found as a suitable loader).
			found = true;
			if (r_error) {
				*r_error = ERR_FILE_NOT_FOUND;
			}
		}
	}
#endif

#ifdef TOOLS_ENABLED
	if (found &&
			Engine::get_singleton()->is_editor_hint() &&
			r_error != nullptr &&
			*r_error == ERR_FILE_NOT_FOUND &&
			ResourceLoader::is_imported(p_path)) {
		return Ref<Resource>();
	}
#endif

	ERR_FAIL_COND_V_MSG(found, Ref<Resource>(), vformat("Failed loading resource: %s.", p_path));

#ifdef TOOLS_ENABLED
	Ref<FileAccess> file_check = FileAccess::create(FileAccess::ACCESS_RESOURCES);
	if (!file_check->file_exists(p_path)) {
		if (r_error) {
			*r_error = ERR_FILE_NOT_FOUND;
		}
		ERR_FAIL_V_MSG(Ref<Resource>(), vformat("Resource file not found: %s (expected type: %s)", p_path, !p_type_hint.is_empty() ? p_type_hint : "unknown"));
	}
#endif

	if (r_error) {
		*r_error = ERR_FILE_UNRECOGNIZED;
	}
	ERR_FAIL_V_MSG(Ref<Resource>(), vformat("No loader found for resource: %s (expected type: %s)", p_path, !p_type_hint.is_empty() ? p_type_hint : "unknown"));
}

// This implementation must allow re-entrancy for a task that started awaiting in a deeper stack frame.
// The load task token must be manually re-referenced before this is called, which includes threaded runs.
void ResourceLoader::_run_load_task(void *p_userdata) {
	ThreadLoadTask &load_task = *(ThreadLoadTask *)p_userdata;
	int thread_index = WorkerThreadPool::get_singleton()->get_thread_index();
	String thread_waiting_on_backup;

	bool wait = false;
	{
		MutexLock thread_load_lock(thread_load_mutex);
		if (cleaning_tasks) {
			load_task.status = THREAD_LOAD_FAILED;
			return;
		}

		if (load_task.started_load && load_task.thread_index != thread_index) {
			// If we were already waiting for task completion in a previous
			// step make sure we don't clobber the old wait.
			if (thread_waiting_on.has(thread_index)) {
				thread_waiting_on_backup = thread_waiting_on[thread_index];
			}

			thread_waiting_on[thread_index] = load_task.local_path;
			wait = true;
		} else {
			load_task.started_load = true;
			load_task.thread_index = thread_index;
		}
	}

	ThreadLoadTask *curr_load_task_backup = curr_load_task;
	curr_load_task = &load_task;

	if (wait) {
		// There are a couple of reasons why we got here:
		// 1) We re-started the task in _load_complete_inner but we also
		//    got started via the original task in the WorkerThreadPool
		// 2) There's a race between multiple threads in _load_complete_inner
		//    and more than one thread thought they had to restart

		ThreadLoadStatus status;
		LocalVector<int> chain;

		do {
			chain.clear();

			thread_load_mutex.lock();

			int waiting_on_thread = load_task.thread_index;
			int current_thread = waiting_on_thread;
			bool progress_blocked = thread_waiting_on.has(waiting_on_thread);

			ThreadLoadTask *waiting_on_task = nullptr;
			bool cycle_detected = false;

			// Try to figure out if we're in a dependency cycle, and what asset
			// we are ultimately waiting for.
			if (progress_blocked && thread_index != -1) {
				while (true) {
					String *waiting_on_path = thread_waiting_on.getptr(current_thread);
					if (!waiting_on_path) {
						break;
					}

					waiting_on_task = thread_load_tasks.getptr(*waiting_on_path);
					if (!waiting_on_task) {
						// Path might be remapped, and someone might be waiting on the
						// remapped path.
						waiting_on_task = thread_load_tasks.getptr(_path_remap(*waiting_on_path));
						if (!waiting_on_task) {
							break;
						}
					}

					// Record the cycle so we can determine whether we should be the one
					// to break it or not.
					int next_thread = waiting_on_task->thread_index;
					chain.push_back(current_thread);

					// We made it back to us, we're in a cycle.
					if (next_thread == thread_index || chain.has(next_thread)) {
						cycle_detected = true;
						break;
					}

					if (chain.size() > thread_load_tasks.size()) {
						ERR_PRINT(vformat("chain.size() > thread_load_tasks.size() for '%s' on thread %d",
								load_task.local_path, thread_index));
						cycle_detected = true;
						break;
					}

					current_thread = next_thread;
				}
			}

			status = load_task.status;

			if (status == THREAD_LOAD_IN_PROGRESS) {
				if (cycle_detected) {
					// Only do something if we're the lowest thread ID waiting,
					// if we didn't we'd run the risk of concurrently running
					// the resource load again.
					int lowest_waiting = thread_index;
					for (const int &link : chain) {
						if (link < lowest_waiting) {
							lowest_waiting = link;
						}
					}

					if (lowest_waiting == thread_index) {
						print_verbose(
								vformat("CYCLE: Stealing on thread %d for resource '%s' originally on thread %d",
										thread_index, load_task.local_path, waiting_on_task->thread_index));
						// Take over the task. The original thread was definitely
						// not going to make progress.
						load_task.thread_index = thread_index;
						thread_waiting_on.erase(thread_index);
						wait = false;
					}
				}
			} else {
				wait = false;
			}

			// Only yield if we ultimately found a task that we are waiting on.
			bool should_yield = progress_blocked && wait && waiting_on_task && thread_index != -1;

			if (should_yield) {
				// We need to make sure we yield on our actual current task. If we are
				// waiting we are certainly not the the task being ran.
				yielders.push_back(WorkerThreadPool::get_singleton()->get_caller_task_id());
			}

			thread_load_mutex.unlock();

			if (should_yield) {
				// We are blocked on some upstream task in our dependency chain. We
				// don't know how long it will take or what is needed to unblock it.
				// If we yield we give the WTP a free thread to solve the problem.
				int load_nesting_backup = load_nesting;
				load_nesting = 0;
				WorkerThreadPool::get_singleton()->yield();
				DEV_ASSERT(load_nesting == 0);
				load_nesting = load_nesting_backup;

				thread_load_mutex.lock();
				yielders.erase(WorkerThreadPool::get_singleton()->get_caller_task_id());
				status = load_task.status;
				thread_load_mutex.unlock();
			} else if (wait) {
				// Forward progress is being made, just wait for task completion.
				// If we are not currently blocked more dependencies might block later,
				// so we cannot yield or wait on a task.
				// This is not the most optimal thing to do, but it is safe. Either the
				// dependency will complete soon, or will block soon when we can safely
				// yield.
				OS::get_singleton()->delay_usec(1000);
			}
		} while (wait && status == THREAD_LOAD_IN_PROGRESS && !cleaning_tasks);

		if (cleaning_tasks || status != THREAD_LOAD_IN_PROGRESS) {
			curr_load_task = curr_load_task_backup;
		}

		if (cleaning_tasks) {
			load_task.status = THREAD_LOAD_FAILED;
			// Do not attempt to unreference the load token. Many things are
			// tearing down concurrently and our task might be dead already. If it is
			// the load token is already released.
			return;
		}

		if (status != THREAD_LOAD_IN_PROGRESS) {
			load_task.load_token->unreference();
			thread_load_mutex.lock();
			if (thread_waiting_on_backup.is_empty()) {
				thread_waiting_on.erase(thread_index);
			} else {
				thread_waiting_on[thread_index] = thread_waiting_on_backup;
			}
			thread_load_mutex.unlock();
			return;
		}

		// do it ourselves anyway
	}

	// Thread-safe either if it's the current thread or a brand new one.
	CallQueue *own_mq_override = nullptr;
	if (load_nesting == 0) {
		if (!Thread::is_main_thread()) {
			// Let the caller thread use its own, for added flexibility. Provide one otherwise.
			if (MessageQueue::get_singleton() == MessageQueue::get_main_singleton()) {
				own_mq_override = memnew(CallQueue);
				MessageQueue::set_thread_singleton_override(own_mq_override);
			}
			set_current_thread_safe_for_nodes(true);
		}
	}
	// --

	bool xl_remapped = false;
	const String &remapped_path = _path_remap(load_task.local_path, &xl_remapped);

	Error load_err = OK;
	Ref<Resource> res = _load(remapped_path, remapped_path != load_task.local_path ? load_task.local_path : String(), load_task.type_hint, load_task.cache_mode, &load_err, load_task.use_sub_threads, &load_task.progress);
	if (MessageQueue::get_singleton() != MessageQueue::get_main_singleton()) {
		MessageQueue::get_singleton()->flush();
	}

	thread_load_mutex.lock();
	bool thread_load_mutex_held = true;

	bool was_finished = load_task.finished_load;
	if (load_task.resource.is_valid()) {
		load_task.finished_load = true;
	}

	load_task.resource = res;

	load_task.progress = 1.0; // It was fully loaded at this point, so force progress to 1.0.
	load_task.error = load_err;

	bool ignoring = load_task.cache_mode == CACHE_MODE_IGNORE || load_task.cache_mode == CACHE_MODE_IGNORE_DEEP;
	bool replacing = load_task.cache_mode == CACHE_MODE_REPLACE || load_task.cache_mode == CACHE_MODE_REPLACE_DEEP;
	if (load_task.resource.is_valid()) {
		// From now on, no critical section needed as no one will write to the task anymore.
		// Moreover, the mutex being unlocked is a requirement if some of the calls below
		// that set the resource up invoke code that in turn requests resource loading.
		thread_load_mutex.unlock();
		thread_load_mutex_held = false;

		if (!ignoring) {
			ResourceCache::lock.lock(); // Check and operations must happen atomically.
			bool pending_unlock = true;
			Ref<Resource> old_res = ResourceCache::get_ref(load_task.local_path);
			if (was_finished) {
				// If another thread already finished the entire load wait for it to complete
				// cache registration, then use their instance.
				while (!old_res.is_valid()) {
					ResourceCache::lock.unlock();
					OS::get_singleton()->delay_usec(1000);
					ResourceCache::lock.lock();
					old_res = ResourceCache::get_ref(load_task.local_path);
				}
			}
			if (old_res.is_valid()) {
				if (old_res != load_task.resource) {
					// Resource can already exists at this point for two reasons:
					// a) The load uses replace mode.
					// b) There were more than one load in flight for the same path because of deadlock prevention.
					// Either case, we want to keep the resource that was already there.
					ResourceCache::lock.unlock();
					pending_unlock = false;
					if (replacing) {
						old_res->copy_from(load_task.resource);
					}
					load_task.resource = old_res;
				}
			} else {
				load_task.resource->set_path(load_task.local_path);
			}
			if (pending_unlock) {
				ResourceCache::lock.unlock();
			}
		} else {
			load_task.resource->set_path_cache(load_task.local_path);
		}

		if (xl_remapped) {
			load_task.resource->set_as_translation_remapped(true);
		}

#ifdef TOOLS_ENABLED
		load_task.resource->set_edited(false);
		if (timestamp_on_load) {
			uint64_t mt = FileAccess::get_modified_time(remapped_path);
			//printf("mt %s: %lli\n",remapped_path.utf8().get_data(),mt);
			load_task.resource->set_last_modified_time(mt);
		}
#endif

		if (_loaded_callback) {
			_loaded_callback(load_task.resource, load_task.local_path);
		}
	} else if (!ignoring) {
		Ref<Resource> existing = ResourceCache::get_ref(load_task.local_path);
		if (existing.is_valid()) {
			load_task.resource = existing;
			load_task.status = THREAD_LOAD_LOADED;
			load_task.progress = 1.0;

			thread_load_mutex.unlock();
			thread_load_mutex_held = false;

			if (_loaded_callback) {
				_loaded_callback(load_task.resource, load_task.local_path);
			}
		}
	}

	if (!thread_load_mutex_held) {
		thread_load_mutex.lock();
	}

	if (cleaning_tasks) {
		// If we are cleaning don't wake up yielders here.
		// And don't unreference the load token, it will get destroyed
		// with the task later.
		load_task.status = THREAD_LOAD_FAILED;
		thread_load_mutex.unlock();
		return;
	}

	if (load_task.error != OK) {
		load_task.status = THREAD_LOAD_FAILED;
	} else {
		load_task.status = THREAD_LOAD_LOADED;
	}

	if (load_task.cond_var && load_task.need_wait) {
		load_task.cond_var->notify_all();
	}
	load_task.need_wait = false;

	if (!thread_waiting_on_backup.is_empty()) {
		thread_waiting_on[thread_index] = thread_waiting_on_backup;
	}

	for (int tid : yielders) {
		// Thundering herd, but not really an issue in practice. The
		// number of threads in the WorkerThreadPool is bound and
		// low.
		WorkerThreadPool::get_singleton()->notify_yield_over(tid);
	}

	thread_load_mutex.unlock();

	// It's safe now to let the task go in case no one else was grabbing the token.
	load_task.load_token->unreference();

	if (load_nesting == 0) {
		if (own_mq_override) {
			MessageQueue::set_thread_singleton_override(nullptr);
			memdelete(own_mq_override);
		}
	}

	curr_load_task = curr_load_task_backup;

	print_verbose(vformat("Completed load for: '%s' remapped '%s' at thread %d", load_task.local_path, remapped_path, thread_index));
}

String ResourceLoader::_validate_local_path(const String &p_path) {
	ResourceUID::ID uid = ResourceUID::get_singleton()->text_to_id(p_path);
	if (uid != ResourceUID::INVALID_ID) {
		return ResourceUID::get_singleton()->get_id_path(uid);
	} else if (p_path.is_relative_path()) {
		return ("res://" + p_path).simplify_path();
	} else {
		return ProjectSettings::get_singleton()->localize_path(p_path);
	}
}

Error ResourceLoader::load_threaded_request(const String &p_path, const String &p_type_hint, bool p_use_sub_threads, CacheMode p_cache_mode) {
	Ref<ResourceLoader::LoadToken> token = _load_start(p_path, p_type_hint, p_use_sub_threads ? LOAD_THREAD_DISTRIBUTE : LOAD_THREAD_SPAWN_SINGLE, p_cache_mode, true);
	return token.is_valid() ? OK : FAILED;
}

ResourceLoader::LoadToken *ResourceLoader::_load_threaded_request_reuse_user_token(const String &p_path) {
	HashMap<String, LoadToken *>::Iterator E = user_load_tokens.find(p_path);
	if (E) {
		print_verbose("load_threaded_request(): Another threaded load for resource path '" + p_path + "' has been initiated. Not an error.");
		LoadToken *token = E->value;
		token->user_rc++;
		return token;
	} else {
		return nullptr;
	}
}

void ResourceLoader::_load_threaded_request_setup_user_token(LoadToken *p_token, const String &p_path) {
	p_token->user_path = p_path;
	p_token->reference(); // Extra RC until all user requests have been gotten.
	p_token->user_rc = 1;
	user_load_tokens[p_path] = p_token;
	print_lt("REQUEST: user load tokens: " + itos(user_load_tokens.size()));
}

Ref<Resource> ResourceLoader::load(const String &p_path, const String &p_type_hint, CacheMode p_cache_mode, Error *r_error) {
	if (r_error) {
		*r_error = OK;
	}

	LoadThreadMode thread_mode = LOAD_THREAD_FROM_CURRENT;
	if (WorkerThreadPool::get_singleton()->get_caller_task_id() != WorkerThreadPool::INVALID_TASK_ID) {
		// If user is initiating a single-threaded load from a WorkerThreadPool task,
		// we instead spawn a new task so there's a precondition that a load in a pool task
		// is always initiated by the engine. That makes certain aspects simpler, such as
		// cyclic load detection and awaiting.
		thread_mode = LOAD_THREAD_SPAWN_SINGLE;
	}
	Ref<LoadToken> load_token = _load_start(p_path, p_type_hint, thread_mode, p_cache_mode);
	if (load_token.is_null()) {
		if (r_error) {
			*r_error = FAILED;
		}
		return Ref<Resource>();
	}

	Ref<Resource> res = _load_complete(*load_token.ptr(), r_error);
	return res;
}

Ref<ResourceLoader::LoadToken> ResourceLoader::_load_start(const String &p_path, const String &p_type_hint, LoadThreadMode p_thread_mode, CacheMode p_cache_mode, bool p_for_user) {
	String local_path = _validate_local_path(p_path);
	ERR_FAIL_COND_V(local_path.is_empty(), Ref<ResourceLoader::LoadToken>());

	bool ignoring_cache = p_cache_mode == CACHE_MODE_IGNORE || p_cache_mode == CACHE_MODE_IGNORE_DEEP;

	Ref<LoadToken> load_token;
	bool must_not_register = false;
	ThreadLoadTask *load_task_ptr = nullptr;
	{
		MutexLock thread_load_lock(thread_load_mutex);

		if (p_for_user) {
			LoadToken *existing_token = _load_threaded_request_reuse_user_token(p_path);
			if (existing_token) {
				return Ref<LoadToken>(existing_token);
			}
		}

		if (!ignoring_cache && thread_load_tasks.has(local_path)) {
			load_token = Ref<LoadToken>(thread_load_tasks[local_path].load_token);
			if (load_token.is_valid()) {
				if (p_for_user) {
					// Load task exists, with no user tokens at the moment.
					// Let's "attach" to it.
					_load_threaded_request_setup_user_token(load_token.ptr(), p_path);
				}
				return load_token;
			} else {
				// The token is dying (reached 0 on another thread).
				// Ensure it's killed now so the path can be safely reused right away.
				thread_load_tasks[local_path].load_token->clear();
			}
		}

		load_token.instantiate();
		load_token->local_path = local_path;
		if (p_for_user) {
			_load_threaded_request_setup_user_token(load_token.ptr(), p_path);
		}

		//create load task
		{
			ThreadLoadTask load_task;

			load_task.load_token = load_token.ptr();
			load_task.local_path = local_path;
			load_task.type_hint = p_type_hint;
			load_task.cache_mode = p_cache_mode;
			load_task.use_sub_threads = p_thread_mode == LOAD_THREAD_DISTRIBUTE;
			if (p_cache_mode == CACHE_MODE_REUSE) {
				Ref<Resource> existing = ResourceCache::get_ref(local_path);
				if (existing.is_valid()) {
					//referencing is fine
					load_task.resource = existing;
					load_task.status = THREAD_LOAD_LOADED;
					load_task.progress = 1.0;
					DEV_ASSERT(!thread_load_tasks.has(local_path));
					thread_load_tasks[local_path] = load_task;
					return load_token;
				}
			}

			// Task hierarchy
			if (curr_load_task) {
				load_task.parent_task = curr_load_task;
				curr_load_task->sub_tasks.insert(load_task.local_path);
			}

			// If we want to ignore cache, but there's another task loading it, we can't add this one to the map.
			must_not_register = ignoring_cache && thread_load_tasks.has(local_path);
			if (must_not_register) {
				load_token->task_if_unregistered = memnew(ThreadLoadTask(load_task));
				load_task_ptr = load_token->task_if_unregistered;
			} else {
				DEV_ASSERT(!thread_load_tasks.has(local_path));
				HashMap<String, ResourceLoader::ThreadLoadTask>::Iterator E = thread_load_tasks.insert(local_path, load_task);
				load_task_ptr = &E->value;
			}
		}

		// It's important to keep the token alive because until the load completes,
		// which includes before the thread start, it may happen that no one is grabbing
		// the token anymore so it's released.
		load_task_ptr->load_token->reference();

		if (p_thread_mode == LOAD_THREAD_FROM_CURRENT) {
			// The current thread may happen to be a thread from the pool.
			WorkerThreadPool::TaskID tid = WorkerThreadPool::get_singleton()->get_caller_task_id();
			if (tid != WorkerThreadPool::INVALID_TASK_ID) {
				load_task_ptr->task_id = tid;
			} else {
				load_task_ptr->thread_id = Thread::get_caller_id();
			}
		} else {
			load_task_ptr->task_id = WorkerThreadPool::get_singleton()->add_native_task(&ResourceLoader::_run_load_task, load_task_ptr);
		}
	} // MutexLock(thread_load_mutex).

	if (p_thread_mode == LOAD_THREAD_FROM_CURRENT) {
		_run_load_task(load_task_ptr);
	}

	return load_token;
}

float ResourceLoader::_dependency_get_progress(const String &p_path) {
	if (thread_load_tasks.has(p_path)) {
		ThreadLoadTask &load_task = thread_load_tasks[p_path];
		if (load_task.in_progress_check) {
			// Given the fact that any resource loaded when an outer stack frame is
			// loading another one is considered a dependency of it, for progress
			// tracking purposes, a cycle can happen if even if the original resource
			// graphs involved have none. For instance, preload() can cause this.
			return load_task.max_reported_progress;
		}
		load_task.in_progress_check = true;
		float current_progress = 0.0;
		int dep_count = load_task.sub_tasks.size();
		if (dep_count > 0) {
			for (const String &E : load_task.sub_tasks) {
				current_progress += _dependency_get_progress(E);
			}
			current_progress /= float(dep_count);
			current_progress *= 0.5;
			current_progress += load_task.progress * 0.5;
		} else {
			current_progress = load_task.progress;
		}
		load_task.max_reported_progress = MAX(load_task.max_reported_progress, current_progress);
		load_task.in_progress_check = false;
		return load_task.max_reported_progress;
	} else {
		return 1.0; //assume finished loading it so it no longer exists
	}
}

ResourceLoader::ThreadLoadStatus ResourceLoader::load_threaded_get_status(const String &p_path, float *r_progress) {
	bool ensure_progress = false;
	ThreadLoadStatus status = THREAD_LOAD_IN_PROGRESS;
	{
		MutexLock thread_load_lock(thread_load_mutex);

		if (!user_load_tokens.has(p_path)) {
			print_verbose("load_threaded_get_status(): No threaded load for resource path '" + p_path + "' has been initiated or its result has already been collected.");
			return THREAD_LOAD_INVALID_RESOURCE;
		}

		String local_path = _validate_local_path(p_path);
		LoadToken *load_token = user_load_tokens[p_path];
		ThreadLoadTask *load_task_ptr;

		if (load_token->task_if_unregistered) {
			load_task_ptr = load_token->task_if_unregistered;
		} else {
			ERR_FAIL_COND_V_MSG(!thread_load_tasks.has(local_path), THREAD_LOAD_INVALID_RESOURCE, "Bug in ResourceLoader logic, please report.");
			load_task_ptr = &thread_load_tasks[local_path];
		}

		status = load_task_ptr->status;
		if (r_progress) {
			*r_progress = _dependency_get_progress(local_path);
		}

		// Support userland polling in a loop on the main thread.
		if (Thread::is_main_thread() && status == THREAD_LOAD_IN_PROGRESS) {
			uint64_t frame = Engine::get_singleton()->get_process_frames();
			if (frame == load_task_ptr->last_progress_check_main_thread_frame) {
				ensure_progress = true;
			} else {
				load_task_ptr->last_progress_check_main_thread_frame = frame;
			}
		}
	}

	if (ensure_progress) {
		_ensure_load_progress();
	}

	return status;
}

Ref<Resource> ResourceLoader::load_threaded_get(const String &p_path, Error *r_error) {
	if (r_error) {
		*r_error = OK;
	}

	Ref<Resource> res;
	{
		MutexLock thread_load_lock(thread_load_mutex);

		if (!user_load_tokens.has(p_path)) {
			print_verbose("load_threaded_get(): No threaded load for resource path '" + p_path + "' has been initiated or its result has already been collected.");
			if (r_error) {
				*r_error = ERR_INVALID_PARAMETER;
			}
			return Ref<Resource>();
		}

		LoadToken *load_token = user_load_tokens[p_path];
		DEV_ASSERT(load_token->user_rc >= 1);

		// Support userland requesting on the main thread before the load is reported to be complete.
		if (Thread::is_main_thread() && !load_token->local_path.is_empty()) {
			ThreadLoadTask *load_task_ptr;

			if (load_token->task_if_unregistered) {
				load_task_ptr = load_token->task_if_unregistered;
			} else {
				if (!thread_load_tasks.has(load_token->local_path)) {
					print_error("Bug in ResourceLoader logic, please report.");
					if (r_error) {
						*r_error = ERR_BUG;
					}
					return Ref<Resource>();
				}

				load_task_ptr = &thread_load_tasks[load_token->local_path];
			}

			while (load_task_ptr->status == THREAD_LOAD_IN_PROGRESS) {
				thread_load_lock.temp_unlock();
				bool exit = !_ensure_load_progress();
				OS::get_singleton()->delay_usec(1000);
				if (MessageQueue::get_singleton()) {
					MessageQueue::get_singleton()->flush();
				}

				thread_load_lock.temp_relock();
				if (exit) {
					break;
				}
			}
		}

		res = _load_complete_inner(*load_token, r_error, thread_load_lock);

		load_token->user_rc--;
		if (load_token->user_rc == 0) {
			load_token->user_path.clear();
			user_load_tokens.erase(p_path);
			if (load_token->unreference()) {
				memdelete(load_token);
				load_token = nullptr;
			}
		}
	}

	print_lt("GET: user load tokens: " + itos(user_load_tokens.size()));

	return res;
}

Ref<Resource> ResourceLoader::_load_complete(LoadToken &p_load_token, Error *r_error) {
	MutexLock thread_load_lock(thread_load_mutex);
	return _load_complete_inner(p_load_token, r_error, thread_load_lock);
}

void ResourceLoader::set_is_import_thread(bool p_import_thread) {
	import_thread = p_import_thread;
}

void ResourceLoader::notify_load_error(const String &p_err) {
	if (err_notify) {
		MessageQueue::get_main_singleton()->push_callable(callable_mp_static(err_notify).bind(p_err));
	}
}

void ResourceLoader::notify_dependency_error(const String &p_path, const String &p_dependency, const String &p_type) {
	if (dep_err_notify) {
		if (Thread::get_caller_id() == Thread::get_main_id()) {
			dep_err_notify(p_path, p_dependency, p_type);
		} else {
			MessageQueue::get_main_singleton()->push_callable(callable_mp_static(dep_err_notify).bind(p_path, p_dependency, p_type));
		}
	}
}

Ref<Resource> ResourceLoader::_load_complete_inner(LoadToken &p_load_token, Error *r_error, MutexLock<SafeBinaryMutex<BINARY_MUTEX_TAG>> &p_thread_load_lock) {
	if (r_error) {
		*r_error = OK;
	}

	ThreadLoadTask *load_task_ptr = nullptr;
	if (p_load_token.task_if_unregistered) {
		load_task_ptr = p_load_token.task_if_unregistered;
	} else {
		if (!thread_load_tasks.has(p_load_token.local_path)) {
			if (r_error) {
				*r_error = ERR_BUG;
			}
			ERR_FAIL_V_MSG(Ref<Resource>(), "Bug in ResourceLoader logic, please report.");
		}

		ThreadLoadTask &load_task = thread_load_tasks[p_load_token.local_path];

		if (load_task.status == THREAD_LOAD_IN_PROGRESS) {
			DEV_ASSERT((load_task.task_id == 0) != (load_task.thread_id == 0));

			if ((load_task.task_id != 0 && load_task.task_id == WorkerThreadPool::get_singleton()->get_caller_task_id()) ||
					(load_task.thread_id != 0 && load_task.thread_id == Thread::get_caller_id())) {
				// Load is in progress, but it's precisely this thread the one in charge.
				// That means this is a cyclic load.
				if (r_error) {
					*r_error = ERR_BUSY;
				}
				return Ref<Resource>();
			}

			bool loader_is_wtp = load_task.task_id != 0;
			if (loader_is_wtp) {
				// Loading thread is in the worker pool.
				p_thread_load_lock.temp_unlock();

				// The wtp won't let us wait on tasks that are older than us. But ResourceLoader has its own
				// deadlock detection and prevention in _run_load_task(), rely on that instead.
				load_task.load_token->reference();
				_run_load_task(&load_task);

				p_thread_load_lock.temp_relock();
				load_task.awaited = true;
				// Mark nested loads with the same task id as awaited.
				for (KeyValue<String, ResourceLoader::ThreadLoadTask> &E : thread_load_tasks) {
					if (E.value.task_id == load_task.task_id) {
						E.value.awaited = true;
					}
				}

				DEV_ASSERT(load_task.status == THREAD_LOAD_FAILED || load_task.status == THREAD_LOAD_LOADED);
			} else if (load_task.need_wait) {
				// Loading thread is main or user thread.
				if (!load_task.cond_var) {
					load_task.cond_var = memnew(ConditionVariable);
				}
				load_task.awaiters_count++;
				do {
					load_task.cond_var->wait(p_thread_load_lock);
					DEV_ASSERT(thread_load_tasks.has(p_load_token.local_path) && p_load_token.get_reference_count());
				} while (load_task.need_wait);
				load_task.awaiters_count--;
				if (load_task.awaiters_count == 0) {
					memdelete(load_task.cond_var);
					load_task.cond_var = nullptr;
				}

				DEV_ASSERT(load_task.status == THREAD_LOAD_FAILED || load_task.status == THREAD_LOAD_LOADED);
			}
		}

		if (cleaning_tasks) {
			load_task.resource = Ref<Resource>();
			load_task.error = FAILED;
		}

		load_task_ptr = &load_task;
	}

	Ref<Resource> resource = load_task_ptr->resource;
	if (r_error) {
		*r_error = load_task_ptr->error;
	}

	if (resource.is_valid()) {
		if (load_task_ptr->parent_task) {
			if (!load_task_ptr->connections_propagated) {
				// A task awaiting another => Let the awaiter accumulate the resource changed connections.
				DEV_ASSERT(load_task_ptr->parent_task != load_task_ptr);
				for (const ThreadLoadTask::ResourceChangedConnection &rcc : load_task_ptr->resource_changed_connections) {
					load_task_ptr->parent_task->resource_changed_connections.push_back(rcc);
				}
				load_task_ptr->connections_propagated = true;
			}
		} else {
			p_thread_load_lock.temp_unlock();

			// A leaf task being awaited => Propagate the resource changed connections.
			if (Thread::is_main_thread()) {
				// On the main thread it's safe to migrate the connections to the standard signal mechanism.
				for (const ThreadLoadTask::ResourceChangedConnection &rcc : load_task_ptr->resource_changed_connections) {
					if (rcc.callable.is_valid()) {
						rcc.source->connect_changed(rcc.callable, rcc.flags);
					}
				}
			} else {
				// On non-main threads, we have to queue and call it done when processed.
				if (!load_task_ptr->resource_changed_connections.is_empty()) {
					for (const ThreadLoadTask::ResourceChangedConnection &rcc : load_task_ptr->resource_changed_connections) {
						if (rcc.callable.is_valid()) {
							MessageQueue::get_main_singleton()->push_callable(callable_mp(rcc.source, &Resource::connect_changed).bind(rcc.callable, rcc.flags));
						}
					}
					if (!import_thread) { // Main thread is blocked by initial resource reimport, do not wait.
						CoreBind::Semaphore done;
						MessageQueue::get_main_singleton()->push_callable(callable_mp(&done, &CoreBind::Semaphore::post).bind(1));
						done.wait();
					}
				}
			}

			p_thread_load_lock.temp_relock();
		}
	}

	return resource;
}

bool ResourceLoader::_ensure_load_progress() {
	// Some servers may need a new engine iteration to allow the load to progress.
	// Since the only known one is the rendering server (in single thread mode), let's keep it simple and just sync it.
	// This may be refactored in the future to support other servers and have less coupling.
	if (OS::get_singleton()->is_separate_thread_rendering_enabled()) {
		return false; // Not needed.
	}
	RenderingServer::get_singleton()->sync();
	return true;
}

void ResourceLoader::resource_changed_connect(Resource *p_source, const Callable &p_callable, uint32_t p_flags) {
	print_lt(vformat("%d\t%ud:%s\t" FUNCTION_STR "\t%d", Thread::get_caller_id(), p_source->get_instance_id(), p_source->get_class(), p_callable.get_object_id()));

	MutexLock lock(thread_load_mutex);

	for (const ThreadLoadTask::ResourceChangedConnection &rcc : curr_load_task->resource_changed_connections) {
		if (unlikely(rcc.source == p_source && rcc.callable == p_callable)) {
			return;
		}
	}

	ThreadLoadTask::ResourceChangedConnection rcc;
	rcc.source = p_source;
	rcc.callable = p_callable;
	rcc.flags = p_flags;
	curr_load_task->resource_changed_connections.push_back(rcc);
	curr_load_task->resource_dependencies.push_back(p_source);
}

void ResourceLoader::resource_changed_disconnect(Resource *p_source, const Callable &p_callable) {
	print_lt(vformat("%d\t%ud:%s\t" FUNCTION_STR "t%d", Thread::get_caller_id(), p_source->get_instance_id(), p_source->get_class(), p_callable.get_object_id()));

	MutexLock lock(thread_load_mutex);

	for (uint32_t i = 0; i < curr_load_task->resource_changed_connections.size(); ++i) {
		const ThreadLoadTask::ResourceChangedConnection &rcc = curr_load_task->resource_changed_connections[i];
		if (unlikely(rcc.source == p_source && rcc.callable == p_callable)) {
			curr_load_task->resource_dependencies.erase(p_source);
			curr_load_task->resource_changed_connections.remove_at_unordered(i);
			return;
		}
	}
}

void ResourceLoader::resource_changed_emit(Resource *p_source) {
	print_lt(vformat("%d\t%ud:%s\t" FUNCTION_STR, Thread::get_caller_id(), p_source->get_instance_id(), p_source->get_class()));

	MutexLock lock(thread_load_mutex);

	for (const ThreadLoadTask::ResourceChangedConnection &rcc : curr_load_task->resource_changed_connections) {
		if (unlikely(rcc.source == p_source)) {
			rcc.callable.call();
		}
	}
}

Ref<Resource> ResourceLoader::ensure_resource_ref_override_for_outer_load(const String &p_path, const String &p_res_type) {
	ERR_FAIL_COND_V(load_nesting == 0, Ref<Resource>()); // It makes no sense to use this from nesting level 0.
	const String &local_path = _validate_local_path(p_path);
	HashMap<String, Ref<Resource>> &overrides = res_ref_overrides[load_nesting - 1];
	HashMap<String, Ref<Resource>>::Iterator E = overrides.find(local_path);
	if (E) {
		return E->value;
	} else {
		Object *obj = ClassDB::instantiate(p_res_type);
		ERR_FAIL_NULL_V(obj, Ref<Resource>());
		Ref<Resource> res(obj);
		if (res.is_null()) {
			memdelete(obj);
			ERR_FAIL_V(Ref<Resource>());
		}
		overrides[local_path] = res;
		return res;
	}
}

Ref<Resource> ResourceLoader::get_resource_ref_override(const String &p_path) {
	DEV_ASSERT(p_path == _validate_local_path(p_path));
	HashMap<int, HashMap<String, Ref<Resource>>>::Iterator E = res_ref_overrides.find(load_nesting);
	if (!E) {
		return nullptr;
	}
	HashMap<String, Ref<Resource>>::Iterator F = E->value.find(p_path);
	if (!F) {
		return nullptr;
	}

	return F->value;
}

bool ResourceLoader::exists(const String &p_path, const String &p_type_hint) {
	String local_path = _validate_local_path(p_path);

	if (ResourceCache::has(local_path)) {
		return true; // If cached, it probably exists
	}

	String path = _path_remap(local_path);

	// Try all loaders and pick the first match for the type hint
	for (int i = 0; i < loader_count; i++) {
		if (!loader[i]->recognize_path(path, p_type_hint)) {
			continue;
		}

		if (loader[i]->exists(path)) {
			return true;
		}
	}

	return false;
}

void ResourceLoader::add_resource_format_loader(Ref<ResourceFormatLoader> p_format_loader, bool p_at_front) {
	ERR_FAIL_COND(p_format_loader.is_null());
	ERR_FAIL_COND(loader_count >= MAX_LOADERS);

	if (p_at_front) {
		for (int i = loader_count; i > 0; i--) {
			loader[i] = loader[i - 1];
		}
		loader[0] = p_format_loader;
		loader_count++;
	} else {
		loader[loader_count++] = p_format_loader;
	}
}

void ResourceLoader::remove_resource_format_loader(Ref<ResourceFormatLoader> p_format_loader) {
	ERR_FAIL_COND(p_format_loader.is_null());

	// Find loader
	int i = 0;
	for (; i < loader_count; ++i) {
		if (loader[i] == p_format_loader) {
			break;
		}
	}

	ERR_FAIL_COND(i >= loader_count); // Not found

	// Shift next loaders up
	for (; i < loader_count - 1; ++i) {
		loader[i] = loader[i + 1];
	}
	loader[loader_count - 1].unref();
	--loader_count;
}

String ResourceLoader::get_import_group_file(const String &p_path) {
	String local_path = _path_remap(_validate_local_path(p_path));

	for (int i = 0; i < loader_count; i++) {
		if (!loader[i]->recognize_path(local_path)) {
			continue;
		}

		return loader[i]->get_import_group_file(p_path);
	}

	return String(); //not found
}

bool ResourceLoader::is_import_valid(const String &p_path) {
	String local_path = _path_remap(_validate_local_path(p_path));

	for (int i = 0; i < loader_count; i++) {
		if (!loader[i]->recognize_path(local_path)) {
			continue;
		}

		return loader[i]->is_import_valid(p_path);
	}

	return false; //not found
}

bool ResourceLoader::is_imported(const String &p_path) {
	String local_path = _path_remap(_validate_local_path(p_path));

	for (int i = 0; i < loader_count; i++) {
		if (!loader[i]->recognize_path(local_path)) {
			continue;
		}

		return loader[i]->is_imported(p_path);
	}

	return false; //not found
}

void ResourceLoader::get_dependencies(const String &p_path, List<String> *p_dependencies, bool p_add_types) {
	String local_path = _path_remap(_validate_local_path(p_path));

	for (int i = 0; i < loader_count; i++) {
		if (!loader[i]->recognize_path(local_path)) {
			continue;
		}

		loader[i]->get_dependencies(local_path, p_dependencies, p_add_types);
	}
}

Error ResourceLoader::rename_dependencies(const String &p_path, const HashMap<String, String> &p_map) {
	String local_path = _path_remap(_validate_local_path(p_path));

	for (int i = 0; i < loader_count; i++) {
		if (!loader[i]->recognize_path(local_path)) {
			continue;
		}

		return loader[i]->rename_dependencies(local_path, p_map);
	}

	return OK; // ??
}

void ResourceLoader::get_classes_used(const String &p_path, HashSet<StringName> *r_classes) {
	String local_path = _validate_local_path(p_path);

	for (int i = 0; i < loader_count; i++) {
		if (!loader[i]->recognize_path(local_path)) {
			continue;
		}

		return loader[i]->get_classes_used(p_path, r_classes);
	}
}

String ResourceLoader::get_resource_type(const String &p_path) {
	String local_path = _validate_local_path(p_path);

	for (int i = 0; i < loader_count; i++) {
		String result = loader[i]->get_resource_type(local_path);
		if (!result.is_empty()) {
			return result;
		}
	}

	return "";
}

String ResourceLoader::get_resource_script_class(const String &p_path) {
	String local_path = _validate_local_path(p_path);

	for (int i = 0; i < loader_count; i++) {
		String result = loader[i]->get_resource_script_class(local_path);
		if (!result.is_empty()) {
			return result;
		}
	}

	return "";
}

ResourceUID::ID ResourceLoader::get_resource_uid(const String &p_path) {
	const String local_path = _validate_local_path(p_path);
	if (!Engine::get_singleton()->is_editor_hint()) {
		return ResourceUID::get_singleton()->get_path_id(local_path);
	}

	for (int i = 0; i < loader_count; i++) {
		ResourceUID::ID id = loader[i]->get_resource_uid(local_path);
		if (id != ResourceUID::INVALID_ID) {
			return id;
		}
	}

	return ResourceUID::INVALID_ID;
}

bool ResourceLoader::should_create_uid_file(const String &p_path) {
	const String local_path = _validate_local_path(p_path);
	if (FileAccess::exists(local_path + ".uid")) {
		return false;
	}

	for (int i = 0; i < loader_count; i++) {
		if (loader[i]->recognize_path(local_path)) {
			return !loader[i]->has_custom_uid_support();
		}
	}
	return false;
}

String ResourceLoader::_path_remap(const String &p_path, bool *r_translation_remapped) {
	String new_path = p_path;

	if (translation_remaps.has(p_path)) {
		// translation_remaps has the following format:
		//   { "res://path.png": PackedStringArray( "res://path-ru.png:ru", "res://path-de.png:de" ) }

		// To find the path of the remapped resource, we extract the locale name after
		// the last ':' to match the project locale.

		// An extra remap may still be necessary afterwards due to the text -> binary converter on export.

		String locale = TranslationServer::get_singleton()->get_locale();
		ERR_FAIL_COND_V_MSG(locale.length() < 2, p_path, vformat("Could not remap path '%s' for translation as configured locale '%s' is invalid.", p_path, locale));

		Vector<String> &res_remaps = *translation_remaps.getptr(new_path);

		int best_score = 0;
		for (int i = 0; i < res_remaps.size(); i++) {
			int split = res_remaps[i].rfind_char(':');
			if (split == -1) {
				continue;
			}
			String l = res_remaps[i].substr(split + 1).strip_edges();
			int score = TranslationServer::get_singleton()->compare_locales(locale, l);
			if (score > 0 && score >= best_score) {
				new_path = res_remaps[i].left(split);
				best_score = score;
				if (score == 10) {
					break; // Exact match, skip the rest.
				}
			}
		}

		if (r_translation_remapped) {
			*r_translation_remapped = true;
		}

		// Fallback to p_path if new_path does not exist.
		if (!FileAccess::exists(new_path + ".import") &&
				!FileAccess::exists(new_path + ".remap") &&
				!FileAccess::exists(new_path)) {
			WARN_PRINT(vformat("Translation remap '%s' does not exist. Falling back to '%s'.", new_path, p_path));
			new_path = p_path;
		}
	}

	// Usually, there's no remap file and FileAccess::exists() is faster than FileAccess::open().
	new_path = ResourceUID::ensure_path(new_path);
	if (FileAccess::exists(new_path + ".remap")) {
		Error err;
		Ref<FileAccess> f = FileAccess::open(new_path + ".remap", FileAccess::READ, &err);
		if (f.is_valid()) {
			VariantParser::StreamFile stream;
			stream.f = f;

			String assign;
			Variant value;
			VariantParser::Tag next_tag;

			int lines = 0;
			String error_text;
			while (true) {
				assign = Variant();
				next_tag.fields.clear();
				next_tag.name = String();

				err = VariantParser::parse_tag_assign_eof(&stream, lines, error_text, next_tag, assign, value, nullptr, true);
				if (err == ERR_FILE_EOF) {
					break;
				} else if (err != OK) {
					ERR_PRINT(vformat("Parse error: %s.remap:%d error: %s.", p_path, lines, error_text));
					break;
				}

				if (assign == "path") {
					new_path = value;
					break;
				} else if (next_tag.name != "remap") {
					break;
				}
			}
		}
	}

	return new_path;
}

String ResourceLoader::import_remap(const String &p_path) {
	if (ResourceFormatImporter::get_singleton()->recognize_path(p_path)) {
		return ResourceFormatImporter::get_singleton()->get_internal_resource_path(p_path);
	}

	return p_path;
}

String ResourceLoader::path_remap(const String &p_path) {
	return _path_remap(p_path);
}

void ResourceLoader::reload_translation_remaps() {
	List<Resource *> to_reload;

	{
		MutexLock lock(ResourceCache::lock);
		SelfList<Resource> *E = remapped_list.first();

		while (E) {
			to_reload.push_back(E->self());
			E = E->next();
		}
	}

	//now just make sure to not delete any of these resources while changing locale..
	while (to_reload.front()) {
		to_reload.front()->get()->reload_from_file();
		to_reload.pop_front();
	}
}

void ResourceLoader::load_translation_remaps() {
	if (!ProjectSettings::get_singleton()->has_setting("internationalization/locale/translation_remaps")) {
		return;
	}

	Dictionary remaps = GLOBAL_GET("internationalization/locale/translation_remaps");
	for (const KeyValue<Variant, Variant> &kv : remaps) {
		Array langs = kv.value;
		Vector<String> lang_remaps;
		lang_remaps.resize(langs.size());
		String *lang_remaps_ptrw = lang_remaps.ptrw();
		for (const Variant &lang : langs) {
			*lang_remaps_ptrw++ = lang;
		}

		translation_remaps[String(kv.key)] = lang_remaps;
	}
}

void ResourceLoader::clear_translation_remaps() {
	translation_remaps.clear();
	while (remapped_list.first() != nullptr) {
		remapped_list.remove(remapped_list.first());
	}
}

void ResourceLoader::clear_thread_load_tasks() {
	// Bring the thing down as quickly as possible without causing deadlocks or leaks.

	MutexLock thread_load_lock(thread_load_mutex);
	cleaning_tasks = true;

	while (true) {
		bool none_running = true;
		for (int tid : yielders) {
			WorkerThreadPool::get_singleton()->notify_yield_over(tid);
		}
		if (thread_load_tasks.size()) {
			for (KeyValue<String, ResourceLoader::ThreadLoadTask> &E : thread_load_tasks) {
				if (E.value.status == THREAD_LOAD_IN_PROGRESS) {
					if (E.value.cond_var && E.value.need_wait) {
						E.value.cond_var->notify_all();
					}
					E.value.need_wait = false;
					none_running = false;
				}
			}
		}
		if (none_running) {
			break;
		}

		thread_load_lock.temp_unlock();

		if (MessageQueue::get_singleton()) {
			MessageQueue::get_singleton()->flush();
		}

		OS::get_singleton()->delay_usec(1000);

		thread_load_lock.temp_relock();
	}

	while (user_load_tokens.begin()) {
		LoadToken *user_token = user_load_tokens.begin()->value;
		user_load_tokens.remove(user_load_tokens.begin());
		DEV_ASSERT(user_token->user_rc > 0 && !user_token->user_path.is_empty());
		user_token->user_path.clear();
		user_token->user_rc = 0;
		user_token->unreference();
	}

	thread_load_tasks.clear();
	thread_waiting_on.clear();
	// yielders is already guaranteed to be empty now

	cleaning_tasks = false;
}

void ResourceLoader::set_load_callback(ResourceLoadedCallback p_callback) {
	_loaded_callback = p_callback;
}

ResourceLoadedCallback ResourceLoader::_loaded_callback = nullptr;

Ref<ResourceFormatLoader> ResourceLoader::_find_custom_resource_format_loader(const String &p_path) {
	for (int i = 0; i < loader_count; ++i) {
		if (loader[i]->get_script_instance() && loader[i]->get_script_instance()->get_script()->get_path() == p_path) {
			return loader[i];
		}
	}
	return Ref<ResourceFormatLoader>();
}

bool ResourceLoader::add_custom_resource_format_loader(const String &p_script_path) {
	if (_find_custom_resource_format_loader(p_script_path).is_valid()) {
		return false;
	}

	Ref<Resource> res = ResourceLoader::load(p_script_path);
	ERR_FAIL_COND_V(res.is_null(), false);
	ERR_FAIL_COND_V(!res->is_class("Script"), false);

	Ref<Script> s = res;
	StringName ibt = s->get_instance_base_type();
	bool valid_type = ClassDB::is_parent_class(ibt, "ResourceFormatLoader");
	ERR_FAIL_COND_V_MSG(!valid_type, false, vformat("Failed to add a custom resource loader, script '%s' does not inherit 'ResourceFormatLoader'.", p_script_path));

	Object *obj = ClassDB::instantiate(ibt);
	ERR_FAIL_NULL_V_MSG(obj, false, vformat("Failed to add a custom resource loader, cannot instantiate '%s'.", ibt));

	Ref<ResourceFormatLoader> crl = Object::cast_to<ResourceFormatLoader>(obj);
	crl->set_script(s);
	ResourceLoader::add_resource_format_loader(crl);

	return true;
}

void ResourceLoader::set_create_missing_resources_if_class_unavailable(bool p_enable) {
	create_missing_resources_if_class_unavailable = p_enable;
}

void ResourceLoader::add_custom_loaders() {
	// Custom loaders registration exploits global class names

	String custom_loader_base_class = ResourceFormatLoader::get_class_static();

	LocalVector<StringName> global_classes;
	ScriptServer::get_global_class_list(global_classes);

	for (const StringName &class_name : global_classes) {
		StringName base_class = ScriptServer::get_global_class_native_base(class_name);

		if (base_class == custom_loader_base_class) {
			String path = ScriptServer::get_global_class_path(class_name);
			add_custom_resource_format_loader(path);
		}
	}
}

void ResourceLoader::remove_custom_loaders() {
	Vector<Ref<ResourceFormatLoader>> custom_loaders;
	for (int i = 0; i < loader_count; ++i) {
		if (loader[i]->get_script_instance()) {
			custom_loaders.push_back(loader[i]);
		}
	}

	for (int i = 0; i < custom_loaders.size(); ++i) {
		remove_resource_format_loader(custom_loaders[i]);
	}
}

bool ResourceLoader::is_cleaning_tasks() {
	MutexLock lock(thread_load_mutex);
	return cleaning_tasks;
}

Vector<String> ResourceLoader::list_directory(const String &p_directory) {
	RBSet<String> files_found;
	Ref<DirAccess> dir = DirAccess::open(p_directory);
	if (dir.is_null()) {
		return Vector<String>();
	}

	Error err = dir->list_dir_begin();
	if (err != OK) {
		return Vector<String>();
	}

	String d = dir->get_next();
	while (!d.is_empty()) {
		bool recognized = false;
		if (dir->current_is_dir()) {
			if (d != "." && d != "..") {
				d += "/";
				recognized = true;
			}
		} else {
			if (d.ends_with(".import") || d.ends_with(".remap") || d.ends_with(".uid")) {
				d = d.substr(0, d.rfind_char('.'));
			}

			if (d.ends_with(".gdc")) {
				d = d.substr(0, d.rfind_char('.'));
				d += ".gd";
			}

			const String full_path = p_directory.path_join(d);
			// Try all loaders and pick the first match for the type hint.
			for (int i = 0; i < loader_count; i++) {
				if (loader[i]->recognize_path(full_path)) {
					recognized = true;
					break;
				}
			}
		}

		if (recognized) {
			files_found.insert(d);
		}
		d = dir->get_next();
	}

	Vector<String> ret;
	for (const String &f : files_found) {
		ret.push_back(f);
	}

	return ret;
}

void ResourceLoader::initialize() {}

void ResourceLoader::finalize() {}

ResourceLoadErrorNotify ResourceLoader::err_notify = nullptr;
DependencyErrorNotify ResourceLoader::dep_err_notify = nullptr;

bool ResourceLoader::create_missing_resources_if_class_unavailable = false;
bool ResourceLoader::abort_on_missing_resource = true;
bool ResourceLoader::timestamp_on_load = false;

thread_local bool ResourceLoader::import_thread = false;
thread_local int ResourceLoader::load_nesting = 0;
thread_local HashMap<int, HashMap<String, Ref<Resource>>> ResourceLoader::res_ref_overrides;
thread_local ResourceLoader::ThreadLoadTask *ResourceLoader::curr_load_task = nullptr;

SafeBinaryMutex<ResourceLoader::BINARY_MUTEX_TAG> &_get_res_loader_mutex() {
	return ResourceLoader::thread_load_mutex;
}

template <>
thread_local SafeBinaryMutex<ResourceLoader::BINARY_MUTEX_TAG>::TLSData SafeBinaryMutex<ResourceLoader::BINARY_MUTEX_TAG>::tls_data(_get_res_loader_mutex());
SafeBinaryMutex<ResourceLoader::BINARY_MUTEX_TAG> ResourceLoader::thread_load_mutex;
HashMap<String, ResourceLoader::ThreadLoadTask> ResourceLoader::thread_load_tasks;
HashMap<int, String> ResourceLoader::thread_waiting_on;
LocalVector<int> ResourceLoader::yielders;

bool ResourceLoader::cleaning_tasks = false;

HashMap<String, ResourceLoader::LoadToken *> ResourceLoader::user_load_tokens;

SelfList<Resource>::List ResourceLoader::remapped_list;
HashMap<String, Vector<String>> ResourceLoader::translation_remaps;

ResourceLoaderImport ResourceLoader::import = nullptr;
