#!/usr/bin/env python3
# Neverhood Reklayed -- generator for engines/neverhood/scenewalker_table.h
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Written 2026 by the Neverhood Reklayed project (see MODIFICATIONS.md at the
# repository root).
#
# Derives every (moduleNum, sceneNum, which) combination the scene walker
# (engines/neverhood/scenewalker.cpp) visits, straight from the engine
# source instead of a hand-written list:
#
#  * moduleNum: every `case NNNN:` in GameModule::createModule() that
#    instantiates a ModuleNNNN (gamemodule.cpp).
#  * sceneNum: every `case N:` in ModuleNNNN::createScene() (modules/*.cpp).
#    Cases that only ever create a Smacker video or a NavigationScene (both
#    video-backed, handled outside the engine via ffmpeg) are dropped; cases
#    that create a Scene, a StaticScene or a module-specific scene helper
#    are kept. The newly-introduced 1000+ sceneNums (video-only by
#    convention) end up dropped by the same rule.
#  * which: -1 (the save-game restore path, reached through the module's own
#    constructor) if the module constructor has one, plus every literal
#    `createScene(N, W)` call in the module file (constructor entrances and
#    updateScene() transitions), plus the Hall of Records link macro's
#    0/1 in module2200.
#
# Usage (from the repository root):
#   python3 devtools/neverhood_scenewalker_table.py > engines/neverhood/scenewalker_table.h

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'engines', 'neverhood')

# Things in a createScene() case that produce a sprite-bearing scene.
SPRITE_SCENE_PATTERNS = [
    r'_childObject\s*=\s*new\s+\w+',
    r'\bcreateStaticScene\s*\(',
    r'\bstatueCloseup\s*\(',
    r'\bcreateScene270[34]\s*\(',
    r'\bcreateHallOfRecordsScene\s*\(',
    r'\bcreateLoadGameMenu\s*\(',
    r'\bcreateSaveGameMenu\s*\(',
    r'\bcreateDeleteGameMenu\s*\(',
]


def function_body(src, signature_regex):
    m = re.search(signature_regex, src)
    if not m:
        return None
    i = src.index('{', m.end())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
    raise ValueError('unbalanced braces after ' + signature_regex)


def split_cases(body):
    """Top-level `case N:` blocks of the first switch in body -> {N: text}."""
    cases = {}
    parts = re.split(r'\n\s*(case\s+-?\d+\s*:|default\s*:)', body)
    current = []
    for part in parts[1:]:
        m = re.match(r'case\s+(-?\d+)\s*:', part)
        if m:
            current.append(int(m.group(1)))
            continue
        if part.startswith('default'):
            current = []
            continue
        text = part
        for n in current:
            cases[n] = text
        # Fallthrough labels (`case 1: case 2: ...`) share the following body:
        # only reset once a real body followed.
        if text.strip():
            current = []
    return cases


def resolve_scene_from_name(class_or_array_name, scene_num_by_class):
    """Scene4-digit embedded in a class or array name (AsScene1202Item,
    kScene1308NumberFileHashes) resolved through scene_num_by_class, or None
    if there is no embedded number or no matching class in this module."""
    m = re.search(r'Scene(\d{4})', class_or_array_name)
    return scene_num_by_class.get('Scene' + m.group(1)) if m else None


def find_extra_hashes(mod, cases, scene_num_by_class):
    """Every literal in a module's `k...FileHash...[]` arrays: puzzle-piece /
    randomized-selection sprites the scene walker's normal construction path
    never triggers a particular one of (the game picks at runtime), collected
    straight from source instead of walked by state. Zero entries (sentinel
    "no piece placed") are skipped, only 0x-prefixed literals count.

    Different scenes in the same module routinely use different palettes
    (module 1300 alone has 7), so exporting these under "whichever scene the
    module happened to walk first" can and does export some of them under
    the wrong one. Resolved by where the array is actually used, in two
    steps, not by guessing from its name (an earlier version tried
    extracting a scene number straight from the array's own name, e.g.
    kScene1308NumberFileHashes to Scene1308; that broke for the several
    arrays named after an old internal scene id that no longer matches any
    real class, e.g. kScene2711FileHashes1..3, actually used inside
    Module2700::createScene()'s case 10, not any "Scene2711" class, which
    does not exist):
      1. If the array's name appears directly inside exactly one
         createScene() switch case's own body, that case's sceneNum owns it
         (kScene2711FileHashes1 above resolves this way, case 10).
      2. Otherwise, find any usage of the array anywhere in the module's
         source and take the nearest preceding `ClassName::method(`; if that
         class is one a switch case directly constructs, that case's
         sceneNum owns it (kScene1308NumberFileHashes resolves this way,
         used inside Scene1308's own constructor rather than inline in the
         switch).
    A handful of arrays resolve neither way (referenced from more than one
    case, or named after an internal class id with no owning scene at all,
    like kAsCommonKeyFileHashes or kClass428FileHashes) and fall back to
    None, exported once per module instead."""
    result = []
    for suffix in ('.cpp', '_sprites.cpp', '_sprites.h'):
        path = os.path.join(ROOT, 'modules', 'module%d%s' % (mod, suffix))
        if not os.path.exists(path):
            continue
        src = open(path, encoding='utf-8').read()
        for m in re.finditer(r'static const uint32 (\w*FileHash\w*)\[\]\s*=\s*\{([^}]*)\}', src):
            name = m.group(1)
            usage_re = re.compile(r'\b' + re.escape(name) + r'\[')

            owning_cases = [scene for scene, text in cases.items() if usage_re.search(text)]
            scene_num = owning_cases[0] if len(owning_cases) == 1 else None

            if scene_num is None:
                # Search from after the array's own declaration (m.end()):
                # searching the whole file would match the declaration
                # itself first (`kFooFileHashes[] = {`, which fits `name\[`
                # too), never reaching any real usage below it.
                um = usage_re.search(src, m.end())
                if um:
                    class_matches = list(re.finditer(r'(\w+)::\w+\s*\(', src[:um.start()]))
                    if class_matches:
                        enclosing_class = class_matches[-1].group(1)
                        scene_num = scene_num_by_class.get(enclosing_class)
                        if scene_num is None:
                            # The array is used inside a helper sprite class
                            # (Ss.../As...) never directly constructed from
                            # the switch, only from inside a scene's own
                            # constructor. Most of those embed the exact
                            # scene they belong to in their own name
                            # (AsScene1202Item -> Scene1202).
                            scene_num = resolve_scene_from_name(enclosing_class, scene_num_by_class)

            if scene_num is None:
                # Last resort: the array's own name (kScene1308NumberFileHashes
                # -> Scene1308). Works for most arrays, but not all: a few are
                # named after an old internal scene id that no longer matches
                # any real class (kScene2711FileHashes1..3, actually used
                # inside Module2700::createScene()'s case 10, resolved above
                # already since it is a direct case-body usage). Never used
                # as anything but a last resort, since it is the least
                # reliable of the three.
                scene_num = resolve_scene_from_name(name, scene_num_by_class)

            for h in re.findall(r'0x[0-9A-Fa-f]+', m.group(2)):
                result.append((int(h, 16), scene_num))
    return result


def main():
    gm = open(os.path.join(ROOT, 'gamemodule.cpp'), encoding='utf-8').read()
    body = function_body(gm, r'void GameModule::createModule\(int moduleNum, int which\)')
    modules = sorted(set(int(n) for n in re.findall(r'_childObject = new Module(\d+)\(', body)))

    entries = []
    summary = []
    extra_entries = []
    for mod in modules:
        path = os.path.join(ROOT, 'modules', 'module%d.cpp' % mod)
        src = open(path, encoding='utf-8').read()
        ctor = function_body(src, r'Module%d::Module%d\(NeverhoodEngine \*vm, Module \*parentModule, int which\)' % (mod, mod))
        cs = function_body(src, r'void Module%d::createScene\(int sceneNum, int which\)' % mod)
        cases = split_cases(cs)
        has_restore = re.search(r'which\s*<\s*0', ctor) is not None and 'gameState().sceneNum' in ctor

        literal = {}
        for s, w in re.findall(r'\bcreateScene\((\d+),\s*(-?\d+)\)', src):
            literal.setdefault(int(s), set()).add(int(w))
        hall = 'HallOfRecordsSceneLink' in src

        kept = dropped = 0
        for scene in sorted(cases):
            text = cases[scene]
            if not any(re.search(p, text) for p in SPRITE_SCENE_PATTERNS):
                dropped += 1
                continue
            kept += 1
            whichs = set(literal.get(scene, set()))
            if has_restore:
                whichs.add(-1)
            if hall and 'createHallOfRecordsScene' in text:
                whichs.update([0, 1])
            if not whichs:
                # Reachable only through a non-literal which; the restore
                # path is the only generic entry, fall back to 0.
                whichs.add(0)
            for w in sorted(whichs):
                entries.append((mod, scene, w))
        summary.append('//   Module%d: %d sprite scene(s), %d video/navigation-only case(s) skipped' % (mod, kept, dropped))

        scene_num_by_class = {}
        for scene, text in cases.items():
            for class_name in re.findall(r'_childObject\s*=\s*new\s+(\w+)\(', text):
                scene_num_by_class[class_name] = scene
        seen = set()
        for h, scene_num in find_extra_hashes(mod, cases, scene_num_by_class):
            key = (mod, scene_num, h)
            if key not in seen:
                seen.add(key)
                extra_entries.append(key)

    # MenuModule (main menu, credits, save/load/delete menus). Not in
    # GameModule::createModule() -- reached through GameModule::createMenuModule().
    mm = open(os.path.join(ROOT, 'menumodule.cpp'), encoding='utf-8').read()
    cs = function_body(mm, r'void MenuModule::createScene\(int sceneNum, int which\)')
    menu_cases = re.findall(r'case (\w+):\s*\n([^\n]*)', cs)
    enum_values = dict((n, int(v)) for n, v in re.findall(r'\b([A-Z_]+)\s*=\s*(\d+)', mm))
    # MAKING_OF is a Smacker video (dropped by the pattern check). The
    # save/load/delete/overwrite screens need "originalsaveload", which the
    # walker forces on for its run.
    menu_scenes = [(name, enum_values[name]) for name, line in menu_cases
                   if any(re.search(p, line) for p in SPRITE_SCENE_PATTERNS)]

    out = sys.stdout
    out.write('''/* ScummVM - Graphic Adventure Engine
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
 * Written 2026 by the Neverhood Reklayed project (see MODIFICATIONS.md).
 * GENERATED by devtools/neverhood_scenewalker_table.py -- do not edit by
 * hand, re-run the generator after changing any createScene() switch.
 */

#ifndef NEVERHOOD_SCENEWALKER_TABLE_H
#define NEVERHOOD_SCENEWALKER_TABLE_H

namespace Neverhood {

// Summary:
''')
    for line in summary:
        out.write(line + '\n')
    out.write('//   MenuModule: %s\n' % ', '.join(n for n, _ in menu_scenes))
    out.write('//   Total: %d (module, sceneNum, which) combination(s) + %d menu scene(s)\n\n' % (len(entries), len(menu_scenes)))
    out.write('struct SceneWalkerEntry {\n\tint moduleNum;\n\tint sceneNum;\n\tint which; // -1 = save-game restore path through the module constructor\n};\n\n')
    out.write('static const SceneWalkerEntry kSceneWalkerEntries[] = {\n')
    for mod, scene, w in entries:
        out.write('\t{ %d, %d, %d },\n' % (mod, scene, w))
    out.write('};\n\n')
    out.write('// MenuModule scene numbers (menumodule.cpp enum), walked via MenuModule itself.\n')
    out.write('static const int kSceneWalkerMenuScenes[] = {\n')
    for name, value in menu_scenes:
        out.write('\t%d, // %s\n' % (value, name))
    out.write('};\n\n')
    with_scene = sum(1 for _, s, _ in extra_entries if s is not None)
    out.write('// Puzzle-piece / randomized-selection sprites from each module\'s own\n')
    out.write('// k...FileHash...[] arrays (%d hash(es), %d resolved to their owning scene\n' % (len(extra_entries), with_scene))
    out.write('// by name (e.g. kScene1308NumberFileHashes -> Scene1308), %d not (shared\n' % (len(extra_entries) - with_scene))
    out.write('// across scenes, or named after an internal class id): the game picks one\n')
    out.write('// at runtime by random or puzzle state, so the normal scene walk only ever\n')
    out.write('// captures whichever one it happened to land on. Exported directly instead,\n')
    out.write('// under the resolved scene\'s own palette when known, or the module\'s first\n')
    out.write('// successful scene otherwise (see exportExtraHashes()).\n')
    out.write('struct SceneWalkerExtraHash {\n\tint moduleNum;\n\tint sceneNum; // -1 = not resolved to a specific scene, see above\n\tuint32 fileHash;\n};\n\n')
    out.write('static const SceneWalkerExtraHash kSceneWalkerExtraHashes[] = {\n')
    for mod, scene_num, h in extra_entries:
        out.write('\t{ %d, %d, 0x%08X },\n' % (mod, -1 if scene_num is None else scene_num, h))
    out.write('};\n\n} // End of namespace Neverhood\n\n#endif\n')


if __name__ == '__main__':
    main()
