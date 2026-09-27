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
 * headless scene walker for the HD source export hook.
 */

#include "audio/mixer.h"

#include "common/config-manager.h"
#include "common/debug.h"
#include "common/events.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/stream.h"
#include "common/system.h"
#include "common/textconsole.h"

#include "neverhood/scenewalker.h"
#include "neverhood/scenewalker_table.h"
#include "neverhood/gamemodule.h"
#include "neverhood/menumodule.h"
#include "neverhood/module.h"
#include "neverhood/resourceman.h"
#include "neverhood/screen.h"
#include "neverhood/sound.h"

namespace Neverhood {

// Pseudo module number for MenuModule groups (not a GameModule module).
static const int kMenuModuleNum = 0;

static const char *const kProgressFileName = "scenewalker_progress.txt";
static const char *const kLogFileName = "scenewalker_log.txt";

// Global vars that are bookkeeping, not asset selectors: flipping them only
// costs time -- or, for the vars that hold a resource hash (Smacker/music/
// Hall of Records column names), feeds a bogus hash of 1 to the engine,
// which crashes on the missing resource.
static const uint32 kIgnoredGlobalVars[] = {
	V_MODULE_NAME, V_CURRENT_SCENE, V_CURRENT_SCENE_WHICH, V_DEBUG,
	V_SMACKER_CAN_ABORT, V_KLAYMEN_FRAMEINDEX, V_KLAYMEN_SAVED_X,
	V_KLAYMEN_IS_DELTA_X, V_CAR_DELTA_X, V_NAVIGATION_INDEX,
	V_CANNON_SMACKER_NAME, V_GOOD_RADIO_MUSIC_NAME, V_UNUSED,
	V_COLUMN_BACK_NAME, V_COLUMN_TEXT_NAME
};

// Arrays that are bookkeeping, same reasoning.
static const uint32 kIgnoredSubVarArrays[] = {
	VA_IS_PUZZLE_INIT, VA_SMACKER_PLAYED
};

// Multi-valued global vars whose values select different art (derived from
// the `== N` comparisons in modules/*.cpp). Every other var is treated as a
// flag and only flipped to 1.
static const struct {
	uint32 nameHash;
	uint32 maxValue;
} kMultiValueGlobalVars[] = {
	{ V_TELEPORTER_WHICH, 5 },
	{ V_TELEPORTER_CURR_LOCATION, 5 },
	{ V_PROJECTOR_LOCATION, 4 },
	{ V_KEY3_LOCATION, 5 },
	{ V_MATCH_STATUS, 3 }
};

SceneWalker::SceneWalker(NeverhoodEngine *vm)
	: _vm(vm), _startMillis(0), _frameCounter(0), _jobCount(0), _fullJobCount(0),
	_skippedJobCount(0), _aborted(false) {

	_exportDir = _vm->_res->getSourceExportDir();
	_drawFrames = ConfMan.hasKey("walk_all_scenes_frames") ? ConfMan.getInt("walk_all_scenes_frames") : 200;
	_settleFrames = ConfMan.hasKey("walk_all_scenes_settle_frames") ? ConfMan.getInt("walk_all_scenes_settle_frames") : 16;
	_maxVariantsPerScene = ConfMan.hasKey("walk_all_scenes_max_variants") ? ConfMan.getInt("walk_all_scenes_max_variants") : 48;
}

bool SceneWalker::run() {
	_startMillis = _vm->_system->getMillis();
	loadProgress();

	// The walk is silent: every scene starts its music/ambience and there
	// is no reason to make the user sit through a few hundred of them.
	static const Audio::Mixer::SoundType kSoundTypes[] = {
		Audio::Mixer::kPlainSoundType, Audio::Mixer::kMusicSoundType,
		Audio::Mixer::kSFXSoundType, Audio::Mixer::kSpeechSoundType
	};
	bool wasMuted[ARRAYSIZE(kSoundTypes)];
	for (uint i = 0; i < ARRAYSIZE(kSoundTypes); i++) {
		wasMuted[i] = _vm->_mixer->isSoundTypeMuted(kSoundTypes[i]);
		_vm->_mixer->muteSoundType(kSoundTypes[i], true);
	}

	// The save/load/delete menus only build their original screens (the art
	// we want) with "originalsaveload" on; otherwise GameStateMenu's
	// constructor opens ScummVM's own modal save/load dialog instead, which
	// blocks the walk on user input and exports nothing. Transient domain:
	// never written back to the user's scummvm.ini.
	Common::ConfigManager::Domain *transient = ConfMan.getDomain(Common::ConfigManager::kTransientDomain);
	const bool hadOriginalSaveLoad = transient->contains("originalsaveload");
	const Common::String prevOriginalSaveLoad = hadOriginalSaveLoad ? transient->getVal("originalsaveload") : Common::String();
	ConfMan.setBool("originalsaveload", true, Common::ConfigManager::kTransientDomain);

	logLine(Common::String::format("scene walker: %d table entries, %d menu scenes, %d settle + %d drawn frames per full job, max %d variants per scene",
		(int)ARRAYSIZE(kSceneWalkerEntries), (int)ARRAYSIZE(kSceneWalkerMenuScenes), _settleFrames, _drawFrames, _maxVariantsPerScene));

	// kSceneWalkerEntries is sorted by (moduleNum, sceneNum): each run of
	// equal (moduleNum, sceneNum) is one group with its "which" values.
	for (uint i = 0; i < ARRAYSIZE(kSceneWalkerEntries) && !_aborted; ) {
		const int moduleNum = kSceneWalkerEntries[i].moduleNum;
		const int sceneNum = kSceneWalkerEntries[i].sceneNum;
		Common::Array<int> whichs;
		for (; i < ARRAYSIZE(kSceneWalkerEntries) && kSceneWalkerEntries[i].moduleNum == moduleNum &&
			kSceneWalkerEntries[i].sceneNum == sceneNum; i++)
			whichs.push_back(kSceneWalkerEntries[i].which);
		// Restore path (-1) first: it is the one save games use, so it is the
		// most robust constructor path and gets the full frame budget.
		if (whichs.size() > 1 && whichs.back() == -1) {
			whichs.pop_back();
			whichs.insert_at(0, -1);
		}
		walkGroup(moduleNum, sceneNum, whichs);
	}

	for (uint i = 0; i < ARRAYSIZE(kSceneWalkerMenuScenes) && !_aborted; i++) {
		Common::Array<int> whichs;
		whichs.push_back(-1);
		walkGroup(kMenuModuleNum, kSceneWalkerMenuScenes[i], whichs);
	}

	teardown();

	for (uint i = 0; i < ARRAYSIZE(kSoundTypes); i++)
		_vm->_mixer->muteSoundType(kSoundTypes[i], wasMuted[i]);

	if (hadOriginalSaveLoad)
		transient->setVal("originalsaveload", prevOriginalSaveLoad);
	else
		transient->erase("originalsaveload");

	const uint32 seconds = (_vm->_system->getMillis() - _startMillis) / 1000;
	if (_aborted) {
		logLine(Common::String::format("scene walker: interrupted after %u s; run again to resume", seconds));
		saveProgress("");
	} else {
		logLine(Common::String::format("scene walker: complete in %u s: %d job(s), %d with full frame budget, %d skipped as failed on an earlier run, %u export key(s) written this run",
			seconds, _jobCount, _fullJobCount, _skippedJobCount, _vm->_res->getExportedKeyCount()));
		for (uint i = 0; i < _failedJobs.size(); i++)
			logLine("scene walker: failed job (crashed on an earlier run): " + _failedJobs[i]);
		Common::String progress;
		for (uint i = 0; i < _failedJobs.size(); i++)
			progress += "FAILED " + _failedJobs[i] + "\n";
		progress += "COMPLETE\n";
		writeTextFile(kProgressFileName, progress);
	}
	writeTextFile(kLogFileName, _log);
	return !_aborted;
}

bool SceneWalker::walkGroup(int moduleNum, int sceneNum, const Common::Array<int> &whichs) {
	const Common::String gid = groupId(moduleNum, sceneNum);
	if (contains(_doneGroups, gid))
		return true;

	_groupSeen.clear();

	// Flip candidates: vars the scene's construction read while they were 0,
	// in first-read order.
	VarSettings candidates;
	Common::HashMap<Common::String, bool> candidateKeys;

	for (uint i = 0; i < whichs.size() && !_aborted; i++) {
		JobResult result;
		runJob(moduleNum, sceneNum, whichs[i], VarSettings(), i == 0, result);
		for (uint r = 0; r < result.reads.size(); r++) {
			const GameVarRead &read = result.reads[r];
			if (read.value != 0 || !isExplorableRead(read))
				continue;
			const Common::String key = varKey(read.nameHash, read.subNameHash, read.isSub);
			if (candidateKeys.contains(key))
				continue;
			candidateKeys[key] = true;
			VarSetting setting = { read.nameHash, read.subNameHash, read.isSub, 0 };
			candidates.push_back(setting);
		}
	}

	// Variants: each candidate flipped on its own, then all flags at once
	// (catches `if (a && b)`), then -- for vars that only get read once
	// another var was flipped (nested branches) -- pairs. All run on the
	// group's first "which". A variant that asks for no bitmap/palette/
	// animation the group has not already seen stops after the settle frames,
	// so exploring a var that turns out not to change any art is cheap.
	Common::Array<VarSettings> queue;
	Common::HashMap<Common::String, bool> queued;
	const int baseWhich = whichs[0];
	for (uint c = 0; c < candidates.size(); c++) {
		GameVarRead asRead = { candidates[c].nameHash, candidates[c].subNameHash, candidates[c].isSub, 0 };
		Common::Array<uint32> values;
		getFlipValues(asRead, values);
		for (uint v = 0; v < values.size(); v++) {
			VarSettings settings;
			VarSetting setting = candidates[c];
			setting.value = values[v];
			settings.push_back(setting);
			queued[jobId(moduleNum, sceneNum, baseWhich, settings)] = true;
			queue.push_back(settings);
		}
	}
	if (candidates.size() > 1) {
		VarSettings all;
		for (uint c = 0; c < candidates.size(); c++) {
			VarSetting setting = candidates[c];
			setting.value = 1;
			all.push_back(setting);
		}
		queue.push_back(all);
	}

	int variants = 0;
	for (uint q = 0; q < queue.size() && !_aborted; q++) {
		if (variants >= _maxVariantsPerScene) {
			logLine(Common::String::format("  %s: variant cap (%d) reached, %d variant(s) not walked", gid.c_str(), _maxVariantsPerScene, (int)(queue.size() - q)));
			break;
		}
		variants++;
		const VarSettings settings = queue[q];
		JobResult result;
		runJob(moduleNum, sceneNum, baseWhich, settings, false, result);
		if (settings.size() != 1)
			continue;
		for (uint r = 0; r < result.reads.size(); r++) {
			const GameVarRead &read = result.reads[r];
			if (read.value != 0 || !isExplorableRead(read))
				continue;
			if (candidateKeys.contains(varKey(read.nameHash, read.subNameHash, read.isSub)))
				continue;
			Common::Array<uint32> values;
			getFlipValues(read, values);
			for (uint v = 0; v < values.size(); v++) {
				VarSettings pair = settings;
				VarSetting setting = { read.nameHash, read.subNameHash, read.isSub, values[v] };
				pair.push_back(setting);
				const Common::String id = jobId(moduleNum, sceneNum, baseWhich, pair);
				if (queued.contains(id))
					continue;
				queued[id] = true;
				queue.push_back(pair);
			}
		}
	}

	if (_aborted)
		return false;
	_doneGroups.push_back(gid);
	saveProgress("");
	return true;
}

bool SceneWalker::runJob(int moduleNum, int sceneNum, int which, const VarSettings &vars, bool forceFull, JobResult &result) {
	result.framesDrawn = 0;
	result.newResources = 0;

	const Common::String id = jobId(moduleNum, sceneNum, which, vars);
	if (contains(_failedJobs, id)) {
		_skippedJobCount++;
		logLine("  " + id + ": SKIPPED (crashed on an earlier run)");
		return false;
	}

	// Written (and flushed) before construction: if anything below takes the
	// whole process down, the next run finds this job as the unfinished one,
	// marks it failed and carries on after it.
	saveProgress(id);
	_jobCount++;
	const uint32 startMillis = _vm->_system->getMillis();
	const uint startKeys = _vm->_res->getExportedKeyCount();

	applyVars(vars);

	Common::HashMap<uint32, bool> queried;
	_vm->_res->setQueryTrace(&queried);
	_vm->_gameVars->setReadTrace(&result.reads);
	constructScene(moduleNum, sceneNum, which);
	_vm->_gameVars->setReadTrace(nullptr);

	// Update-only frames first: lets palette fades (fade-in from black,
	// Scene1004's area crossfade, ...) settle before anything is drawn, so
	// the export hook's first writes already carry the final colors.
	for (int f = 0; f < _settleFrames && !_aborted; f++)
		pumpFrame(false);

	for (Common::HashMap<uint32, bool>::const_iterator it = queried.begin(); it != queried.end(); ++it)
		if (!_groupSeen.contains(it->_key))
			result.newResources++;

	const bool full = forceFull || result.newResources > 0;
	if (full) {
		_fullJobCount++;
		for (int f = 0; f < _drawFrames && !_aborted; f++) {
			pumpFrame(true);
			result.framesDrawn++;
		}
	}

	_vm->_res->setQueryTrace(nullptr);
	for (Common::HashMap<uint32, bool>::const_iterator it = queried.begin(); it != queried.end(); ++it)
		_groupSeen[it->_key] = true;

	teardown();

	logLine(Common::String::format("  %s: %s, %d new visual resource(s), %d frame(s) drawn, +%u export key(s), %u ms",
		id.c_str(), full ? "walked" : "no new art, probe only", result.newResources, result.framesDrawn,
		_vm->_res->getExportedKeyCount() - startKeys, _vm->_system->getMillis() - startMillis));
	return true;
}

void SceneWalker::constructScene(int moduleNum, int sceneNum, int which) {
	GameModule *gameModule = _vm->_gameModule;

	if (moduleNum == kMenuModuleNum) {
		// MenuModule's constructor always opens the main menu; any other menu
		// scene replaces it, like MenuModule::updateScene() does.
		gameModule->_childObject = new MenuModule(_vm, gameModule, 0);
		Module *menuModule = (Module *)gameModule->_childObject;
		if (sceneNum != 0) {
			delete menuModule->_childObject;
			menuModule->_childObject = nullptr;
			menuModule->walkerCreateScene(sceneNum, which);
		}
		return;
	}

	// Same path as loading a save game: the module constructor, called with
	// which = -1, rebuilds gameState().sceneNum (and, for Module2500/2700,
	// reads gameState().which).
	_vm->gameState().sceneNum = sceneNum;
	_vm->gameState().which = which < 0 ? 0 : which;
	gameModule->createModule(moduleNum, -1);

	if (which >= 0 || moduleNum == 2900) {
		// A specific entrance: replace the restored scene with the one the
		// module's own transition code would build (Module::updateChild()
		// deletes the old scene the same way before calling createScene()).
		// Module2900's constructor ignores the restore path and always builds
		// scene 0, hence the explicit replace there too.
		Module *module = (Module *)gameModule->_childObject;
		delete module->_childObject;
		module->_childObject = nullptr;
		module->walkerCreateScene(sceneNum, which);
	}
}

void SceneWalker::pumpFrame(bool draw) {
	// Input is dropped (nothing in a scene should react to the user's mouse
	// during the walk), but polling keeps the window responsive and lets a
	// quit request end the walk cleanly.
	Common::Event event;
	while (_vm->_system->getEventManager()->pollEvent(event)) {
	}
	if (_vm->shouldQuit()) {
		_aborted = true;
		return;
	}

	// Only the scene is updated, not its module: the module's updateScene()
	// would react to the scene finishing (leaveScene()) by building the next
	// scene, which is the walker's job, not the module's.
	Module *module = (Module *)_vm->_gameModule->_childObject;
	if (module && module->_childObject)
		module->_childObject->handleUpdate();

	if (draw) {
		_vm->_gameModule->draw();
		_vm->_screen->update();
	}

	// Presenting every frame would tie the walk to the display refresh rate.
	if ((++_frameCounter % 16) == 0)
		_vm->_system->updateScreen();
}

void SceneWalker::teardown() {
	GameModule *gameModule = _vm->_gameModule;
	delete gameModule->_childObject;
	gameModule->_childObject = nullptr;
	// Same cleanup as a game restore (GameModule::checkRequests()). Without
	// it, looping ambience started by a few hundred scenes piles up in the
	// mixer (muted, but still holding channels) until it runs out of slots.
	_vm->_audioResourceMan->stopAllMusic();
	_vm->_audioResourceMan->stopAllSounds();
	_vm->_soundMan->stopAllMusic();
	_vm->_soundMan->stopAllSounds();
	_vm->_mixer->stopAll();
}

void SceneWalker::applyVars(const VarSettings &vars) {
	// Every job starts from a fresh game's state (what "new game" does,
	// see GameModule::checkRequests()), plus the variant's own settings.
	_vm->_gameVars->clear();
	for (uint i = 0; i < vars.size(); i++) {
		if (vars[i].isSub)
			_vm->_gameVars->setSubVar(vars[i].nameHash, vars[i].subNameHash, vars[i].value);
		else
			_vm->_gameVars->setGlobalVar(vars[i].nameHash, vars[i].value);
	}
}

bool SceneWalker::isExplorableRead(const GameVarRead &read) {
	if (read.isSub) {
		for (uint i = 0; i < ARRAYSIZE(kIgnoredSubVarArrays); i++)
			if (read.nameHash == kIgnoredSubVarArrays[i])
				return false;
	} else {
		for (uint i = 0; i < ARRAYSIZE(kIgnoredGlobalVars); i++)
			if (read.nameHash == kIgnoredGlobalVars[i])
				return false;
	}
	return true;
}

void SceneWalker::getFlipValues(const GameVarRead &read, Common::Array<uint32> &values) {
	values.clear();
	if (!read.isSub) {
		for (uint i = 0; i < ARRAYSIZE(kMultiValueGlobalVars); i++) {
			if (read.nameHash == kMultiValueGlobalVars[i].nameHash) {
				for (uint32 v = 1; v <= kMultiValueGlobalVars[i].maxValue; v++)
					values.push_back(v);
				return;
			}
		}
	}
	values.push_back(1);
}

Common::String SceneWalker::varKey(uint32 nameHash, uint32 subNameHash, bool isSub) {
	return isSub ? Common::String::format("S%08X.%08X", nameHash, subNameHash) : Common::String::format("G%08X", nameHash);
}

Common::String SceneWalker::groupId(int moduleNum, int sceneNum) {
	if (moduleNum == kMenuModuleNum)
		return Common::String::format("menu:%d", sceneNum);
	return Common::String::format("%d:%d", moduleNum, sceneNum);
}

Common::String SceneWalker::jobId(int moduleNum, int sceneNum, int which, const VarSettings &vars) {
	Common::String id = groupId(moduleNum, sceneNum) + Common::String::format(":%d", which);
	for (uint i = 0; i < vars.size(); i++)
		id += "+" + varKey(vars[i].nameHash, vars[i].subNameHash, vars[i].isSub) + Common::String::format("=%u", vars[i].value);
	return id;
}

// Progress file (next to the exported PNGs), rewritten before every job:
//   DONE <group>     scene group finished, skipped on resume
//   FAILED <job>     job took the process down on an earlier run, skipped
//   CURRENT <job>    job in progress; still present at startup = it crashed
//   COMPLETE         whole walk finished; the next run starts over
// A crash (segfault, or error() from a scene that can't be built headlessly)
// ends the process; running the walker again resumes after the offending job.
void SceneWalker::loadProgress() {
	Common::String text;
	if (!readTextFile(kProgressFileName, text))
		return;
	bool complete = false;
	Common::String crashedJob;
	Common::Array<Common::String> done, failed;
	const char *p = text.c_str();
	while (*p) {
		const char *end = strchr(p, '\n');
		Common::String line = end ? Common::String(p, end) : Common::String(p);
		p = end ? end + 1 : p + strlen(p);
		line.trim();
		if (line.hasPrefix("DONE "))
			done.push_back(line.substr(5));
		else if (line.hasPrefix("FAILED "))
			failed.push_back(line.substr(7));
		else if (line.hasPrefix("CURRENT "))
			crashedJob = line.substr(8);
		else if (line == "COMPLETE")
			complete = true;
	}
	if (complete) {
		// Fresh walk; failures stay known so they don't crash this run too.
		_failedJobs = failed;
		return;
	}
	_doneGroups = done;
	_failedJobs = failed;
	readTextFile(kLogFileName, _log);
	if (!crashedJob.empty() && !contains(_failedJobs, crashedJob)) {
		_failedJobs.push_back(crashedJob);
		logLine("scene walker: previous run died in job " + crashedJob + ", marked failed");
	}
	logLine(Common::String::format("scene walker: resuming, %d scene group(s) already done", (int)_doneGroups.size()));
}

void SceneWalker::saveProgress(const Common::String &currentJob) {
	Common::String text;
	for (uint i = 0; i < _doneGroups.size(); i++)
		text += "DONE " + _doneGroups[i] + "\n";
	for (uint i = 0; i < _failedJobs.size(); i++)
		text += "FAILED " + _failedJobs[i] + "\n";
	if (!currentJob.empty())
		text += "CURRENT " + currentJob + "\n";
	writeTextFile(kProgressFileName, text);
	if (currentJob.empty())
		writeTextFile(kLogFileName, _log);
}

void SceneWalker::writeTextFile(const char *name, const Common::String &text) {
	Common::DumpFile out;
	if (!out.open(_exportDir.appendComponent(name), true)) {
		warning("scene walker: cannot write %s", name);
		return;
	}
	out.write(text.c_str(), text.size());
	out.flush();
	out.close();
}

bool SceneWalker::readTextFile(const char *name, Common::String &text) {
	const Common::FSNode node(_exportDir.appendComponent(name));
	if (!node.exists())
		return false;
	Common::SeekableReadStream *in = node.createReadStream();
	if (!in)
		return false;
	text.clear();
	char buf[1024];
	uint32 n;
	while ((n = in->read(buf, sizeof(buf))) > 0)
		text += Common::String(buf, n);
	delete in;
	return true;
}

void SceneWalker::logLine(const Common::String &line) {
	debug(1, "%s", line.c_str());
	_log += line + "\n";
}

bool SceneWalker::contains(const Common::Array<Common::String> &list, const Common::String &item) {
	for (uint i = 0; i < list.size(); i++)
		if (list[i] == item)
			return true;
	return false;
}

} // End of namespace Neverhood
