/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Modified 2026 by the Neverhood Reklayed project (see MODIFICATIONS.md):
 * added the HD asset override lookup (initOverrides/hasOverride/getOverride)
 * and the HD source export hook (exportSource).
 */

#include "common/ptr.h"
#include "image/png.h"
#include "neverhood/resourceman.h"

namespace Neverhood {

ResourceHandle::ResourceHandle()
	: _resourceFileEntry(nullptr), _data(nullptr) {
}

ResourceHandle::~ResourceHandle() {
}

ResourceMan::ResourceMan() {
}

ResourceMan::~ResourceMan() {
	for (Common::HashMap<Common::String, Graphics::Surface*>::iterator it = _overrideSurfaces.begin(); it != _overrideSurfaces.end(); ++it) {
		if (it->_value) {
			it->_value->free();
			delete it->_value;
		}
	}
}

// HD override layer (Neverhood Reklayed)

static Common::String makeOverrideKey(uint32 fileHash, int frameIndex) {
	if (frameIndex < 0)
		return Common::String::format("%08X", fileHash);
	return Common::String::format("%08X_%d", fileHash, frameIndex);
}

// Parses "<8 hex digits>.png" or "<8 hex digits>_<decimal>.png" (case-insensitive).
static bool parseOverrideFilename(const Common::String &name, uint32 &fileHash, int &frameIndex) {
	if (name.size() < 12 || !name.hasSuffixIgnoreCase(".png"))
		return false;
	const Common::String stem(name.c_str(), name.size() - 4);
	fileHash = 0;
	for (uint i = 0; i < 8; i++) {
		const char c = stem[i];
		uint digit;
		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'a' && c <= 'f')
			digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			digit = c - 'A' + 10;
		else
			return false;
		fileHash = (fileHash << 4) | digit;
	}
	if (stem.size() == 8) {
		frameIndex = -1;
		return true;
	}
	if (stem[8] != '_' || stem.size() == 9 || stem.size() > 13)
		return false;
	frameIndex = 0;
	for (uint i = 9; i < stem.size(); i++) {
		if (stem[i] < '0' || stem[i] > '9')
			return false;
		frameIndex = frameIndex * 10 + (stem[i] - '0');
	}
	return true;
}

int ResourceMan::initOverrides(const Common::FSNode &dir) {
	Common::FSList files;
	if (!dir.getChildren(files, Common::FSNode::kListFilesOnly))
		return 0;
	for (Common::FSList::const_iterator it = files.begin(); it != files.end(); ++it) {
		uint32 fileHash;
		int frameIndex;
		if (!parseOverrideFilename(it->getName(), fileHash, frameIndex))
			continue;
		_overrideFiles[makeOverrideKey(fileHash, frameIndex)] = *it;
		_overrideHashes[fileHash] = true;
		debug(2, "ResourceMan::initOverrides() found %s", it->getName().c_str());
	}
	return _overrideFiles.size();
}

const Graphics::Surface *ResourceMan::getOverride(uint32 fileHash, int frameIndex) {
	if (!hasOverride(fileHash))
		return nullptr;
	const Common::String key = makeOverrideKey(fileHash, frameIndex);
	if (_overrideSurfaces.contains(key))
		return _overrideSurfaces[key];
	if (!_overrideFiles.contains(key))
		return nullptr;

	Graphics::Surface *result = nullptr;
	Common::ScopedPtr<Common::SeekableReadStream> stream(_overrideFiles[key].createReadStream());
	Image::PNGDecoder decoder;
	if (stream && decoder.loadStream(*stream) && decoder.getSurface()) {
		const Graphics::Surface *decoded = decoder.getSurface();
		const Graphics::Palette &palette = decoder.getPalette();
		result = decoded->convertTo(getOverrideFormat(), palette.size() ? palette.data() : nullptr, palette.size());
		debug(1, "HD override: loaded %s.png (%dx%d)", key.c_str(), result->w, result->h);
	} else {
		warning("HD override: failed to decode %s.png, falling back to the original asset", key.c_str());
	}
	_overrideSurfaces[key] = result;
	return result;
}

bool ResourceMan::hasOverride(uint32 fileHash) const {
	return _overrideHashes.contains(fileHash);
}

// HD source export (Neverhood Reklayed)

void ResourceMan::exportSource(uint32 fileHash, int frameIndex, const Graphics::Surface &surface, const byte *paletteData, bool transparent, byte alphaColor, bool flipX, bool flipY) {
	if (!_sourceExportEnabled)
		return;
	if (!paletteData) {
		debug(3, "HD source export: skipped %s (no active palette yet)", makeOverrideKey(fileHash, frameIndex).c_str());
		return;
	}

	// Re-write a few times, not once: the palette can still be mid-fade the
	// first time an asset is drawn (e.g. a scene fading in over its
	// background), so a single first-draw snapshot can catch the wrong
	// colors. A handful of draws later the fade has settled; stop after
	// that instead of writing every frame for the rest of the session.
	static const int kMaxExportsPerKey = 5;
	const Common::String key = makeOverrideKey(fileHash, frameIndex);
	int &exportCount = _exportedKeys[key];
	if (exportCount >= kMaxExportsPerKey)
		return;
	exportCount++;

	byte rgbPalette[768];
	for (int i = 0; i < 256; i++) {
		rgbPalette[i * 3 + 0] = paletteData[i * 4 + 0];
		rgbPalette[i * 3 + 1] = paletteData[i * 4 + 1];
		rgbPalette[i * 3 + 2] = paletteData[i * 4 + 2];
	}

	Common::DumpFile out;
	if (!out.open(_sourceExportDir.appendComponent(key + ".png"), true)) {
		warning("HD source export: failed to open %s.png for writing", key.c_str());
		return;
	}

	if (transparent) {
		// Build real per-pixel alpha instead of relying on writePNG's palette
		// path, which has no way to mark one index as transparent -- the
		// "background" color Screen::blitRenderItem would have skipped would
		// otherwise end up as an opaque solid color in the PNG. Also
		// un-mirrors back to the canonical orientation, see exportSource()'s
		// declaration comment.
		Graphics::Surface rgba;
		rgba.create(surface.w, surface.h, Graphics::PixelFormat::createFormatRGBA32());
		for (int y = 0; y < surface.h; y++) {
			const byte *src = (const byte *)surface.getBasePtr(0, flipY ? surface.h - 1 - y : y);
			uint32 *dst = (uint32 *)rgba.getBasePtr(0, y);
			for (int x = 0; x < surface.w; x++) {
				const byte index = src[flipX ? surface.w - 1 - x : x];
				if (index == alphaColor)
					dst[x] = rgba.format.ARGBToColor(0, 0, 0, 0);
				else
					dst[x] = rgba.format.ARGBToColor(255, rgbPalette[index * 3 + 0], rgbPalette[index * 3 + 1], rgbPalette[index * 3 + 2]);
			}
		}
		Image::writePNG(out, rgba, nullptr, 0);
		rgba.free();
	} else if (flipX || flipY) {
		Graphics::Surface mirrored;
		mirrored.create(surface.w, surface.h, surface.format);
		for (int y = 0; y < surface.h; y++) {
			const byte *src = (const byte *)surface.getBasePtr(0, flipY ? surface.h - 1 - y : y);
			byte *dst = (byte *)mirrored.getBasePtr(0, y);
			for (int x = 0; x < surface.w; x++)
				dst[x] = src[flipX ? surface.w - 1 - x : x];
		}
		Image::writePNG(out, mirrored, rgbPalette, 256);
		mirrored.free();
	} else {
		Image::writePNG(out, surface, rgbPalette, 256);
	}

	out.close();
	debug(2, "HD source export: wrote %s.png", key.c_str());
}

void ResourceMan::addArchive(const Common::Path &filename, bool isOptional) {
	BlbArchive *archive = new BlbArchive();
	if (!archive->open(filename, isOptional)) {
		delete archive;
		return;
	}
	_archives.push_back(archive);
	debug(3, "ResourceMan::addArchive(%s) %d files", filename.toString(Common::Path::kNativeSeparator).c_str(), archive->getCount());
	for (uint archiveEntryIndex = 0; archiveEntryIndex < archive->getCount(); archiveEntryIndex++) {
		BlbArchiveEntry *archiveEntry = archive->getEntry(archiveEntryIndex);
		ResourceFileEntry *entry = findEntrySimple(archiveEntry->fileHash);
		if (entry) {
			if (entry->archiveEntry == nullptr || archiveEntry->timeStamp > entry->archiveEntry->timeStamp) {
				entry->archive = archive;
				entry->archiveEntry = archiveEntry;
			}
		} else {
			ResourceFileEntry newEntry;
			newEntry.resourceHandle = -1;
			newEntry.archive = archive;
			newEntry.archiveEntry = archiveEntry;
			newEntry.nhcArchive = nullptr;
			newEntry.nhcArchiveEntry = nullptr;
			_entries[archiveEntry->fileHash] = newEntry;
		}
	}
}

bool ResourceMan::addNhcArchive(const Common::Path &filename) {
	NhcArchive *archive = new NhcArchive();
	if (!archive->open(filename, true)) {
		delete archive;
		return false;
	}
	_nhcArchives.push_back(archive);
	debug(3, "ResourceMan::addArchive(%s) %d files", filename.toString(Common::Path::kNativeSeparator).c_str(), archive->getCount());
	for (uint archiveEntryIndex = 0; archiveEntryIndex < archive->getCount(); archiveEntryIndex++) {
		NhcArchiveEntry *archiveEntry = archive->getEntry(archiveEntryIndex);
		ResourceFileEntry *entry = findEntrySimple(archiveEntry->fileHash);
		if (entry) {
			entry->nhcArchive = archive;
			entry->nhcArchiveEntry = archiveEntry;
		} else {
			ResourceFileEntry newEntry;
			newEntry.resourceHandle = -1;
			newEntry.archive = nullptr;
			newEntry.archiveEntry = nullptr;
			newEntry.nhcArchive = archive;
			newEntry.nhcArchiveEntry = archiveEntry;
			_entries[archiveEntry->fileHash] = newEntry;
		}
	}

	return true;
}

ResourceFileEntry *ResourceMan::findEntrySimple(uint32 fileHash) {
	EntriesMap::iterator p = _entries.find(fileHash);
	return p != _entries.end() ? &p->_value : nullptr;
}

ResourceFileEntry *ResourceMan::findEntry(uint32 fileHash, ResourceFileEntry **firstEntry) {
	ResourceFileEntry *entry = findEntrySimple(fileHash);
	if (firstEntry)
		*firstEntry = entry;
	for (; entry && entry->archiveEntry != nullptr && entry->archiveEntry->comprType == 0x65; fileHash = entry->archiveEntry->diskSize)
		entry = findEntrySimple(fileHash);
	return entry;
}

Common::SeekableReadStream *ResourceMan::createStream(uint32 fileHash) {
	ResourceFileEntry *entry = findEntry(fileHash);
	if (!entry)
		return nullptr;
	if (entry->nhcArchiveEntry && entry->nhcArchive && entry->nhcArchiveEntry->isNormal())
		return entry->nhcArchive->createStream(entry->nhcArchiveEntry);
	if (entry->archiveEntry && entry->archive)
		return entry->archive->createStream(entry->archiveEntry);
	return nullptr;
}

Common::SeekableReadStream *ResourceMan::createNhcStream(uint32 fileHash, uint32 type) {
	ResourceFileEntry *entry = findEntry(fileHash);
	if (!entry)
		return nullptr;
	if (entry->nhcArchiveEntry && entry->nhcArchive && entry->nhcArchiveEntry->type == type)
		return entry->nhcArchive->createStream(entry->nhcArchiveEntry);
	return nullptr;
}

bool ResourceMan::nhcExists(uint32 fileHash, uint32 type) {
	ResourceFileEntry *entry = findEntry(fileHash);
	if (!entry)
		return false;
	if (entry->nhcArchiveEntry && entry->nhcArchive && entry->nhcArchiveEntry->type == type)
		return true;
	return false;
}

bool ResourceMan::exists(uint32 fileHash) {
	ResourceFileEntry *entry = findEntry(fileHash);
	if (!entry)
		return false;
	if (entry->nhcArchiveEntry && entry->nhcArchive && entry->nhcArchiveEntry->isNormal())
		return true;
	if (entry->archiveEntry && entry->archive)
		return true;
	return false;
}

void ResourceMan::queryResource(uint32 fileHash, ResourceHandle &resourceHandle) {
	ResourceFileEntry *firstEntry;
	resourceHandle._resourceFileEntry = findEntry(fileHash, &firstEntry);
	resourceHandle._extData = firstEntry && firstEntry->archiveEntry ? firstEntry->archiveEntry->extData : nullptr;
}

struct EntrySizeFix {
	uint32 fileHash;
	uint32 offset;
	uint32 diskSize;
	uint32 size;
	uint32 fixedSize;
};

static const EntrySizeFix entrySizeFixes[] = {
	//  fileHash    offset   diskSize  size  fixedSize
	// Fixes for the Russian "Dyadyushka Risech" version
	{ 0x041137051,   667019,  23391,  41398,  29191 },	// "Options" menu header text
	{ 0x00f960021,   402268,   1704,   4378,   1870 },	// "Save" menu
	{ 0x01301a7ea,  1220008,   2373,   4146,   2877 },	// "Load" menu
	{ 0x084181e81,   201409,   1622,   5058,   1833 },	// "Delete" menu
	{ 0x0C10B2015,   690410,   5850,  11162,   7870 },	// Menu text
	{ 0x008C0AC24,  1031009,   3030,   6498,   3646 },	// Overwrite dialog
	{ 0x0c6604282, 12813649,  19623,  35894,  30370 },	// One of the fonts when reading Willie's notes
	{ 0x080283101, 13104841,   1961,   3712,   3511 },	// First message from Willie
	{ 0x058208810, 46010519,  24852, 131874, 114762 },  // Entry to hut with musical lock
	{ 0x000918480, 17676417,    581,    916,    706 },	// First wall in the museum
	{ 0x00800090C, 16064875,  19555,  38518,  30263 },	// First wall in the museum
	{ 0x00008E486, 39600019,    240,    454,    271 },  // Second wall in the museum
	{ 0x003086004, 39621755,    482,    614,    600 },  // Second wall in the museum
	{ 0x02008048E, 39611075,   3798,  21089,   6374 },  // Next walls in the museum
	{ 0x008586283, 39587864,  12155,  29731,  20582 },  // Next walls in the museum
	{ 0x030A84C80, 39606142,   4933,  16305,   8770 },  // Next walls in the museum
	{ 0x000C9A480, 39614873,   6882,  23915,  11571 },  // Next walls in the museum
	{ 0x000098880, 39603114,   3028,  10860,   4762 },  // Next walls in the museum
	{ 0x040080183, 39600259,   2855,  13400,   4305 },  // Next walls in the museum
	{ 0x004290188, 39580567,   7297,  27131,  12322 },  // Next walls in the museum
	{ 0x0283CE401, 12795150,  18499,  36658,  29166 },  // Late-game notes

	// Fixes for the Russian "Fargus" version
	{ 0x041137051,   758264,  29037,  49590,  49591 },	// "Options" menu header text
	{ 0x0c10b2015,   787304,   4414,  15848,  15853 },	// Text on option buttons
	{ 0x006802920,  1076824,   1010,   5642,   1546 },	// Crash in front of the Aqua House
	//
	{          0,        0,         0,     0,         0 }
};

void ResourceMan::loadResource(ResourceHandle &resourceHandle, bool applyResourceFixes) {
	resourceHandle._data = nullptr;
	if (resourceHandle.isValid()) {
		const uint32 fileHash = resourceHandle.fileHash();

		if (hasOverride(fileHash))
			debug(3, "ResourceMan::loadResource() HD override available for %08x", fileHash);

		ResourceData *resourceData = _data[fileHash];
		if (!resourceData) {
			resourceData = new ResourceData();
			_data[fileHash] = resourceData;
		}
		if (resourceData->data != nullptr) {
			resourceData->dataRefCount++;
		} else {
			NhcArchiveEntry *nhcEntry = resourceHandle._resourceFileEntry->nhcArchiveEntry;
			if (nhcEntry && nhcEntry->isNormal()) {
				resourceData->data = new byte[nhcEntry->size];
				resourceHandle._resourceFileEntry->nhcArchive->load(nhcEntry, resourceData->data, 0);
			} else {
				BlbArchiveEntry *entry = resourceHandle._resourceFileEntry->archiveEntry;

				// Apply fixes for broken resources in Russian versions
				if (applyResourceFixes) {
					for (const EntrySizeFix *cur = entrySizeFixes; cur->fileHash > 0; ++cur) {
						if (entry->fileHash == cur->fileHash && entry->offset == cur->offset &&
						    entry->diskSize == cur->diskSize && entry->size == cur->size)
							entry->size = cur->fixedSize;
					}
				}

				resourceData->data = new byte[entry->size];
				resourceHandle._resourceFileEntry->archive->load(entry, resourceData->data, 0);
			}
			resourceData->dataRefCount = 1;
		}
		resourceHandle._data = resourceData->data;
	}
}

void ResourceMan::unloadResource(ResourceHandle &resourceHandle) {
	if (resourceHandle.isValid()) {
		ResourceData *resourceData = _data[resourceHandle.fileHash()];
		if (resourceData && resourceData->dataRefCount > 0)
			--resourceData->dataRefCount;
		resourceHandle._resourceFileEntry = nullptr;
		resourceHandle._data = nullptr;
	}
}

void ResourceMan::purgeResources() {
	for (Common::HashMap<uint32, ResourceData*>::iterator it = _data.begin(); it != _data.end(); ++it) {
		ResourceData *resourceData = it->_value;
		if (resourceData->dataRefCount == 0) {
			delete[] resourceData->data;
			resourceData->data = nullptr;
		}
	}
}

} // End of namespace Neverhood
