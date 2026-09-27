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
 * added an optional true-color HD composite path next to the CLUT8 backbuffer.
 */

#ifndef NEVERHOOD_SCREEN_H
#define NEVERHOOD_SCREEN_H

#include "common/array.h"
#include "common/hashmap.h"
#include "common/hash-ptr.h"
#include "graphics/surface.h"
#include "neverhood/neverhood.h"
#include "neverhood/microtiles.h"
#include "neverhood/graphics.h"

namespace Video {
	class SmackerDecoder;
}

namespace Neverhood {

struct RenderItem {
	const Graphics::Surface *_surface;
	const Graphics::Surface *_shadowSurface;
	int16 _destX, _destY;
	int16 _srcX, _srcY, _width, _height;
	bool _transparent;
	byte _version;
	bool _refresh;
	byte _alphaColor;
	bool operator==(const RenderItem &second) const {
		return
			_surface == second._surface &&
			_shadowSurface == second._shadowSurface &&
			_destX == second._destX &&
			_destY == second._destY &&
			_srcX == second._srcX &&
			_srcY == second._srcY &&
			_width == second._width &&
			_height == second._height &&
			_transparent == second._transparent &&
			_version == second._version &&
			_alphaColor == second._alphaColor;
	}
};

typedef Common::Array<RenderItem> RenderQueue;

// Neverhood Reklayed: HD override bound to one CLUT8 BaseSurface surface.
// width/height are the original sprite/frame size in 640x480 space; the
// original is always unpacked at the surface origin, mirrored by flipX/flipY.
struct HdBinding {
	const Graphics::Surface *hdSurface; // ResourceMan::getOverrideFormat()
	int16 width, height;
	bool flipX, flipY;
};

// Neverhood Reklayed: whole-screen palette fades mirrored onto true-color HD
// pixels, since those no longer have palette indices. See Palette::update().
struct HdFade {
	enum Mode { kNone, kToColor, kFromColor };
	Mode mode;
	byte r, g, b;
	int amount; // 0..255, per-channel step toward (kToColor) or away from (kFromColor) r/g/b
	HdFade() : mode(kNone), r(0), g(0), b(0), amount(0) {}
};

class Screen {
public:
	Screen(NeverhoodEngine *vm);
	~Screen();
	void update();
	uint32 getNextFrameTime();
	void saveParams();
	void restoreParams();
	void setFps(int fps);
	int getFps();
	void setYOffset(int16 yOffset);
	int16 getYOffset();
	void setPaletteData(byte *paletteData);
	void unsetPaletteData(byte *paletteData);
	byte *getPaletteData() { return _paletteData; }
	void testPalette(byte *paletteData);
	void updatePalette();
	void clear();
	void clearRenderQueue();
	void drawSurface2(const Graphics::Surface *surface, NDrawRect &drawRect, NRect &clipRect, bool transparent, byte version,
			  const Graphics::Surface *shadowSurface = NULL, byte alphaColor = 0);
	void drawSurface3(const Graphics::Surface *surface, int16 x, int16 y, NDrawRect &drawRect, NRect &clipRect, bool transparent, byte version);
	void drawDoubleSurface2(const Graphics::Surface *surface, NDrawRect &drawRect);
	void drawDoubleSurface2Alpha(const Graphics::Surface *surface, NDrawRect &drawRect, byte alphaColor);
	void drawUnk(const Graphics::Surface *surface, NDrawRect &drawRect, NDrawRect &sysRect, NRect &clipRect, bool transparent, byte version);
	void drawSurfaceClipRects(const Graphics::Surface *surface, NDrawRect &drawRect, NRect *clipRects, uint clipRectsCount, bool transparent, byte version);
	void setSmackerDecoder(Video::SmackerDecoder *smackerDecoder) { _smackerDecoder = smackerDecoder; }
	void queueBlit(const Graphics::Surface *surface, int16 destX, int16 destY, NRect &ddRect, bool transparent, byte version,
		       const Graphics::Surface *shadowSurface = NULL, byte alphaColor = 0);
	void blitRenderItem(const RenderItem &renderItem, const Common::Rect &clipRect);
	// Neverhood Reklayed HD path. Off unless enableHd() is called; when off,
	// none of this code runs and the output is the upstream CLUT8 640x480 one.
	static const int kHdScale = 2;
	void enableHd(const Graphics::PixelFormat &format);
	bool isHdEnabled() const { return _hdScreen != nullptr; }
	int getOutputScale() const { return _hdScreen ? kHdScale : 1; }
	void setHdBinding(const Graphics::Surface *surface, const Graphics::Surface *hdSurface, int16 width, int16 height, bool flipX, bool flipY);
	void clearHdBinding(const Graphics::Surface *surface);
	// Called by Palette for whole-palette fades; ignored unless HD is enabled
	// and paletteData is the palette currently shown.
	void hdFadeOutStep(const byte *paletteData, byte r, byte g, byte b, int step);
	void hdFadeInStart(const byte *paletteData, bool fromUniformColor, byte r, byte g, byte b);
	void hdFadeInStep(const byte *paletteData, int step);
	void hdFadeInDone(const byte *paletteData);
protected:
	Graphics::Surface *_hdScreen;
	uint32 _hdPalette[256];
	bool _hdPaletteDirty;
	HdFade _hdFade;
	Common::HashMap<const Graphics::Surface*, HdBinding> _hdBindings;
	void updateHdPalette();
	void convertBackScreenToHd(const Common::Rect &rect);
	void blitRenderItemHd(const RenderItem &renderItem, const Common::Rect &clipRect);
	void copyHdRectToScreen(const Common::Rect &rect);
	uint32 applyHdFade(byte r, byte g, byte b) const;
	void putHdPixel(int x, int y, uint32 color);
	NeverhoodEngine *_vm;
	MicroTileArray *_microTiles;
	Graphics::Surface *_backScreen;
	Video::SmackerDecoder *_smackerDecoder, *_savedSmackerDecoder;
	int32 _ticks;
	int32 _frameDelay, _savedFrameDelay;
	byte *_paletteData;
	bool _paletteChanged;
	int16 _yOffset, _savedYOffset;
	bool _fullRefresh;
	RenderQueue *_renderQueue, *_prevRenderQueue;
};

} // End of namespace Neverhood

#endif /* NEVERHOOD_SCREEN_H */
