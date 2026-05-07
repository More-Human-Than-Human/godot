/**************************************************************************/
/*  resource_importer_texture_settings.cpp                                */
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

#include "resource_importer_texture_settings.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"

namespace {

static bool _source_matches_patterns(const String &p_source_file, const String &p_patterns_csv) {
	const String source_lower = p_source_file.to_lower();
	const PackedStringArray patterns = p_patterns_csv.split(",", false);
	for (const String &pattern_raw : patterns) {
		const String pattern = pattern_raw.strip_edges().to_lower();
		if (!pattern.is_empty() && source_lower.findn(pattern) != -1) {
			return true;
		}
	}
	return false;
}

} // namespace

// ResourceImporterTextureSettings contains code used by
// multiple texture importers and the export dialog.
bool ResourceImporterTextureSettings::use_footprint_policy() {
	return bool(GLOBAL_GET("editor/import/texture/footprint_policy_enabled"));
}

bool ResourceImporterTextureSettings::is_data_map_path(const String &p_source_file) {
	return _source_matches_patterns(p_source_file, GLOBAL_GET("editor/import/texture/data_map_name_patterns"));
}

bool ResourceImporterTextureSettings::should_disable_mipmaps_for_data_maps() {
	return bool(GLOBAL_GET("editor/import/texture/disable_mipmaps_for_data_maps"));
}

int ResourceImporterTextureSettings::get_footprint_size_limit(bool p_is_3d_texture, bool p_is_data_map) {
	if (!use_footprint_policy()) {
		return 0;
	}

	int size_limit = p_is_3d_texture ? int(GLOBAL_GET("editor/import/texture/max_size_3d")) : int(GLOBAL_GET("editor/import/texture/max_size_2d"));
	if (p_is_data_map) {
		const int data_map_limit = int(GLOBAL_GET("editor/import/texture/max_size_data_maps"));
		if (data_map_limit > 0 && (size_limit <= 0 || data_map_limit < size_limit)) {
			size_limit = data_map_limit;
		}
	}

	if (size_limit < 0) {
		size_limit = 0;
	}
	return size_limit;
}

bool ResourceImporterTextureSettings::should_import_s3tc_bptc() {
	if (GLOBAL_GET("rendering/textures/vram_compression/import_s3tc_bptc")) {
		return true;
	}
	// If the project settings override is not enabled, import
	// S3TC/BPTC only when the host operating system needs it.
	return OS::get_singleton()->get_preferred_texture_format() == OS::PREFERRED_TEXTURE_FORMAT_S3TC_BPTC;
}

bool ResourceImporterTextureSettings::should_import_etc2_astc() {
	if (GLOBAL_GET("rendering/textures/vram_compression/import_etc2_astc")) {
		return true;
	}
	// If the project settings override is not enabled, import
	// ETC2/ASTC only when the host operating system needs it.
	return OS::get_singleton()->get_preferred_texture_format() == OS::PREFERRED_TEXTURE_FORMAT_ETC2_ASTC;
}
