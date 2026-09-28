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
 * Written 2026 by the Neverhood Reklayed project (see MODIFICATIONS.md):
 * headless scene walker for the HD source export hook, plus a direct export
 * pass for each module's puzzle-piece file-hash arrays.
 */

#ifndef NEVERHOOD_SCENEWALKER_H
#define NEVERHOOD_SCENEWALKER_H

#include "common/array.h"
#include "common/hashmap.h"
#include "common/path.h"
#include "common/str.h"
#include "neverhood/neverhood.h"
#include "neverhood/gamevars.h"

namespace Neverhood {

// Neverhood Reklayed: non-interactive pre-pass that constructs every sprite
// scene of the game in turn (table: scenewalker_table.h, generated from the
// createScene() switches) and pumps the normal update/draw/render path for a
// while, so the HD source export hook (export_hd_source_path) captures every
// scene's backgrounds, sprites and animation frames with the scene's own
// palette -- no human playthrough needed. See MODIFICATIONS.md for the full
// description, settings and known gaps.
class SceneWalker {
public:
	SceneWalker(NeverhoodEngine *vm);
	// Returns false if the walk was interrupted (engine quit requested).
	bool run();

private:
	// One global var (isSub=false) or array element (isSub=true) forced to a
	// value before a scene is constructed.
	struct VarSetting {
		uint32 nameHash;
		uint32 subNameHash;
		bool isSub;
		uint32 value;
	};
	typedef Common::Array<VarSetting> VarSettings;

	struct JobResult {
		Common::Array<GameVarRead> reads; // during construction only
		int framesDrawn;
		int newResources;
	};

	NeverhoodEngine *_vm;
	Common::Path _exportDir;
	int _drawFrames;
	int _settleFrames;
	int _maxVariantsPerScene;

	// Visual resources (bitmap/palette/animation hashes) already seen in the
	// scene group currently being walked.
	Common::HashMap<uint32, bool> _groupSeen;

	// Resume support: see loadProgress().
	Common::Array<Common::String> _doneGroups;
	Common::Array<Common::String> _failedJobs;
	Common::String _log;

	uint32 _startMillis;
	uint _frameCounter;
	int _jobCount, _fullJobCount, _skippedJobCount;
	bool _aborted;

	// (moduleNum, sceneNum) pairs whose kSceneWalkerExtraHashes entries have
	// already been exported this run (see exportExtraHashes()). sceneNum -1
	// is the module-level fallback bucket (entries not resolved to a
	// specific scene), tracked the same way as any other "scene".
	Common::HashMap<Common::String, bool> _extraHashesDone;

	bool walkGroup(int moduleNum, int sceneNum, const Common::Array<int> &whichs);
	bool runJob(int moduleNum, int sceneNum, int which, const VarSettings &vars, bool forceFull, JobResult &result);
	void constructScene(int moduleNum, int sceneNum, int which);
	// Neverhood Reklayed: exports every kSceneWalkerExtraHashes entry whose
	// (moduleNum, sceneNum) matches exactly, using whatever palette is
	// currently active. Called twice per completed job, from runJob(): once
	// with sceneNum -1 (the module-level fallback bucket, once per module)
	// and once with the job's real sceneNum (that scene's own resolved
	// entries, once per scene). Different scenes in the same module can use
	// different palettes, see the table generator and MODIFICATIONS.md.
	// These are puzzle-piece / randomized-selection sprites (module
	// k...FileHash...[] arrays) that the
	// game picks between at runtime, so the normal walk only ever sees
	// whichever one the current job's state happened to select.
	void exportExtraHashes(int moduleNum, int sceneNum);
	void pumpFrame(bool draw);
	void teardown();

	void applyVars(const VarSettings &vars);
	static bool isExplorableRead(const GameVarRead &read);
	static void getFlipValues(const GameVarRead &read, Common::Array<uint32> &values);
	static Common::String varKey(uint32 nameHash, uint32 subNameHash, bool isSub);
	static Common::String groupId(int moduleNum, int sceneNum);
	static Common::String jobId(int moduleNum, int sceneNum, int which, const VarSettings &vars);

	void loadProgress();
	void saveProgress(const Common::String &currentJob);
	void writeTextFile(const char *name, const Common::String &text);
	bool readTextFile(const char *name, Common::String &text);
	void logLine(const Common::String &line);
	static bool contains(const Common::Array<Common::String> &list, const Common::String &item);
};

} // End of namespace Neverhood

#endif /* NEVERHOOD_SCENEWALKER_H */
