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
 * and the HD source export hook (enableSourceExport/exportSource), which
 * writes real per-pixel alpha for transparent sprites. Resource query trace
 * (setQueryTrace) and export accessors for the headless scene walker.
 */

#ifndef NEVERHOOD_RESOURCEMAN_H
#define NEVERHOOD_RESOURCEMAN_H

#include "common/array.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/hashmap.h"
#include "common/hash-str.h"
#include "common/path.h"
#include "graphics/surface.h"
#include "neverhood/neverhood.h"
#include "neverhood/blbarchive.h"
#include "neverhood/nhcarchive.h"

namespace Neverhood {

class ResourceMan;
struct ResourceHandle;

class ResourceFileEntry {
private:
	int resourceHandle;
	BlbArchive *archive;
	BlbArchiveEntry *archiveEntry;

	NhcArchive *nhcArchive;
	NhcArchiveEntry *nhcArchiveEntry;

	friend struct ResourceHandle;
	friend class ResourceMan;

public:
	ResourceFileEntry() : resourceHandle(-1), archive(nullptr), archiveEntry(nullptr), nhcArchive(nullptr), nhcArchiveEntry(nullptr) {}
};

struct Resource {
	ResourceFileEntry *entry;
	int useRefCount;
};

struct ResourceData {
	byte *data;
	int dataRefCount;
	ResourceData() : data(NULL), dataRefCount() {}
};

struct ResourceHandle {
friend class ResourceMan;
public:
	ResourceHandle();
	~ResourceHandle();
	bool isValid() const { return _resourceFileEntry != NULL
			&& (_resourceFileEntry->archiveEntry != NULL
			    || (_resourceFileEntry->nhcArchiveEntry != NULL && _resourceFileEntry->nhcArchiveEntry->isNormal())); }
	byte type() const {
		if (_resourceFileEntry == NULL)
			return 0;
		if (_resourceFileEntry->nhcArchiveEntry != NULL && _resourceFileEntry->nhcArchiveEntry->isNormal())
			return _resourceFileEntry->nhcArchiveEntry->type;
		if (_resourceFileEntry->archiveEntry != NULL)
			return _resourceFileEntry->archiveEntry->type;
		return 0;
	}
	const byte *data() const { return _data; }
	uint32 size() const {
		if (_resourceFileEntry == NULL)
			return 0;
		if (_resourceFileEntry->nhcArchiveEntry != NULL && _resourceFileEntry->nhcArchiveEntry->isNormal())
			return _resourceFileEntry->nhcArchiveEntry->size;
		if (_resourceFileEntry->archiveEntry != NULL)
			return _resourceFileEntry->archiveEntry->size;
		return 0;
	}
	const byte *extData() const { return _extData; }
	uint32 fileHash() const {
		if (_resourceFileEntry == NULL)
			return 0;
		if (_resourceFileEntry->nhcArchiveEntry != NULL && _resourceFileEntry->nhcArchiveEntry->isNormal())
			return _resourceFileEntry->nhcArchiveEntry->fileHash;
		if (_resourceFileEntry->archiveEntry != NULL)
			return _resourceFileEntry->archiveEntry->fileHash;
		return 0;
	}
protected:
	ResourceFileEntry *_resourceFileEntry;
	const byte *_extData;
	const byte *_data;
};

class ResourceMan {
public:
	ResourceMan();
	~ResourceMan();
	void addArchive(const Common::Path &filename, bool isOptional = false);
	bool addNhcArchive(const Common::Path &filename);
	ResourceFileEntry *findEntrySimple(uint32 fileHash);
	ResourceFileEntry *findEntry(uint32 fileHash, ResourceFileEntry **firstEntry = NULL);
	Common::SeekableReadStream *createStream(uint32 fileHash);
	// Decompressed bytes for fileHash in a fresh, caller-owned buffer
	// (caller deletes[] it), bypassing the shared _data cache entirely.
	// nullptr if fileHash doesn't exist.
	byte *readResourceUncached(uint32 fileHash, uint32 &outSize);
	Common::SeekableReadStream *createNhcStream(uint32 fileHash, uint32 type);
	bool nhcExists(uint32 fileHash, uint32 type);
	bool exists(uint32 fileHash);
	// HD asset override layer (Neverhood Reklayed), see MODIFICATIONS.md.
	// initOverrides() scans one directory for "<HASH>.png" (sprite) and
	// "<HASH>_<frameIndex>.png" (animation frame) files; returns how many it found.
	int initOverrides(const Common::FSNode &dir);
	bool hasOverride(uint32 fileHash) const;
	// Decoded override in getOverrideFormat(), or nullptr if there is none.
	// frameIndex < 0 = sprite, otherwise animation frame. Decoded lazily, then cached.
	const Graphics::Surface *getOverride(uint32 fileHash, int frameIndex = -1);
	static Graphics::PixelFormat getOverrideFormat() { return Graphics::PixelFormat::createFormatARGB32(); }
	// HD source export (Neverhood Reklayed), see MODIFICATIONS.md.
	// Opt-in: while enabled, every drawn sprite/anim frame is dumped once as
	// "<HASH>.png" / "<HASH>_<frameIndex>.png" using the CLUT8 pixels and the
	// palette active at the moment it was drawn (a raw index has no fixed
	// color outside that context). Meant to be run through actual gameplay,
	// not a static archive walk, so exported colors are always correct.
	void enableSourceExport(const Common::Path &dir) { _sourceExportDir = dir; _sourceExportEnabled = true; }
	bool isSourceExportEnabled() const { return _sourceExportEnabled; }
	// transparent/alphaColor mirror what Screen::blitRenderItem would use for
	// this surface, so the exported PNG carries real alpha instead of a solid
	// color for the "transparent" palette index (matters even for a single
	// stray pixel, e.g. inside a letter's counter shape).
	// flipX/flipY: surface currently holds a mirrored draw; un-mirrored back
	// to the canonical orientation before writing, so a flipped and unflipped
	// draw of the same asset can't corrupt each other's export.
	void exportSource(uint32 fileHash, int frameIndex, const Graphics::Surface &surface, const byte *paletteData, bool transparent, byte alphaColor = 0, bool flipX = false, bool flipY = false);
	const Common::Path &getSourceExportDir() const { return _sourceExportDir; }
	uint getExportedKeyCount() const { return _exportedKeys.size(); }
	// Scene walker (Neverhood Reklayed): while non-null, the hash of every
	// queryResource() call (i.e. every resource a scene or sprite asks for)
	// is inserted into trace.
	void setQueryTrace(Common::HashMap<uint32, bool> *trace) { _queryTrace = trace; }
	const ResourceFileEntry& getEntry(uint index) { return _entries[index]; }
	uint getEntryCount() { return _entries.size(); }
	void queryResource(uint32 fileHash, ResourceHandle &resourceHandle);
	void loadResource(ResourceHandle &resourceHandle, bool applyResourceFixes);
	void unloadResource(ResourceHandle &resourceHandle);
	void purgeResources();
protected:
	typedef Common::HashMap<uint32, ResourceFileEntry> EntriesMap;
	Common::Array<BlbArchive*> _archives;
	Common::Array<NhcArchive*> _nhcArchives;
	EntriesMap _entries;
	Common::HashMap<uint32, ResourceData*> _data;
	Common::Array<Resource*> _resources;
	// HD override layer. Empty unless initOverrides() was called.
	Common::HashMap<uint32, bool> _overrideHashes;
	Common::HashMap<Common::String, Common::FSNode> _overrideFiles;
	Common::HashMap<Common::String, Graphics::Surface*> _overrideSurfaces; // nullptr value = decode failed
	// HD source export.
	bool _sourceExportEnabled = false;
	Common::Path _sourceExportDir;
	Common::HashMap<Common::String, int> _exportedKeys; // export count per key, see exportSource()
	Common::HashMap<uint32, bool> *_queryTrace = nullptr;
};

} // End of namespace Neverhood

#endif /* NEVERHOOD_RESOURCEMAN_H */
