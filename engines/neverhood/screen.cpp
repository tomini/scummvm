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

#include "graphics/cursorman.h"
#include "graphics/paletteman.h"
#include "video/smk_decoder.h"
#include "neverhood/screen.h"

namespace Neverhood {

Screen::Screen(NeverhoodEngine *vm)
	: _vm(vm), _paletteData(nullptr), _paletteChanged(false), _smackerDecoder(nullptr),
	_yOffset(0), _fullRefresh(false), _frameDelay(0), _savedSmackerDecoder(nullptr),
	_savedFrameDelay(0), _savedYOffset(0), _hdScreen(nullptr), _hdPaletteDirty(false) {

	memset(_hdPalette, 0, sizeof(_hdPalette));

	_ticks = _vm->_system->getMillis();

	_backScreen = new Graphics::Surface();
	_backScreen->create(640, 480, Graphics::PixelFormat::createFormatCLUT8());

	_renderQueue = new RenderQueue();
	_prevRenderQueue = new RenderQueue();
	_microTiles = new MicroTileArray(640, 480);

}

Screen::~Screen() {
	delete _microTiles;
	delete _renderQueue;
	delete _prevRenderQueue;
	_backScreen->free();
	delete _backScreen;
	if (_hdScreen) {
		_hdScreen->free();
		delete _hdScreen;
	}
}

void Screen::update() {
	_ticks = _vm->_system->getMillis();
	updatePalette();

	if (_fullRefresh) {
		// NOTE When playing a fullscreen/doubled Smacker video usually a full screen refresh is needed
		if (_hdScreen) {
			const Common::Rect fullRect(0, 0, 640, 480);
			convertBackScreenToHd(fullRect);
			_hdPaletteDirty = false;
			copyHdRectToScreen(fullRect);
		} else
			_vm->_system->copyRectToScreen((const byte*)_backScreen->getPixels(), _backScreen->pitch, 0, 0, 640, 480);
		_fullRefresh = false;
		return;
	}

	_microTiles->clear();

	for (RenderQueue::iterator it = _renderQueue->begin(); it != _renderQueue->end(); ++it) {
		RenderItem &renderItem = (*it);
		renderItem._refresh = true;
		for (RenderQueue::iterator jt = _prevRenderQueue->begin(); jt != _prevRenderQueue->end(); ++jt) {
			RenderItem &prevRenderItem = (*jt);
			if (prevRenderItem == renderItem) {
				prevRenderItem._refresh = false;
				renderItem._refresh = false;
			}
		}
	}

	for (RenderQueue::iterator jt = _prevRenderQueue->begin(); jt != _prevRenderQueue->end(); ++jt) {
		RenderItem &prevRenderItem = (*jt);
		if (prevRenderItem._refresh)
			_microTiles->addRect(Common::Rect(prevRenderItem._destX, prevRenderItem._destY, prevRenderItem._destX + prevRenderItem._width, prevRenderItem._destY + prevRenderItem._height));
	}

	for (RenderQueue::iterator it = _renderQueue->begin(); it != _renderQueue->end(); ++it) {
		RenderItem &renderItem = (*it);
		if (renderItem._refresh)
			_microTiles->addRect(Common::Rect(renderItem._destX, renderItem._destY, renderItem._destX + renderItem._width, renderItem._destY + renderItem._height));
		renderItem._refresh = true;
	}

	if (_hdScreen && _hdPaletteDirty) {
		// Colors are baked into the true-color buffer at blit time, so a palette
		// change means re-deriving everything: convert what is on the CLUT8
		// backbuffer, then re-blit every queued item (restores HD pixels in z-order).
		const Common::Rect fullRect(0, 0, 640, 480);
		convertBackScreenToHd(fullRect);
		_microTiles->addRect(fullRect);
		_hdPaletteDirty = false;
	}

	RectangleList *updateRects = _microTiles->getRectangles();

	for (RenderQueue::iterator it = _renderQueue->begin(); it != _renderQueue->end(); ++it) {
		RenderItem &renderItem = (*it);
		for (RectangleList::iterator ri = updateRects->begin(); ri != updateRects->end(); ++ri)
			blitRenderItem(renderItem, *ri);
	}

	SWAP(_renderQueue, _prevRenderQueue);
	_renderQueue->clear();

	for (RectangleList::iterator ri = updateRects->begin(); ri != updateRects->end(); ++ri) {
		Common::Rect &r = *ri;
		if (_hdScreen)
			copyHdRectToScreen(r);
		else
			_vm->_system->copyRectToScreen((const byte*)_backScreen->getBasePtr(r.left, r.top), _backScreen->pitch, r.left, r.top, r.width(), r.height());
	}

	delete updateRects;

}

uint32 Screen::getNextFrameTime() {
	int32 frameDelay = _frameDelay;
	if (_smackerDecoder && _smackerDecoder->isVideoLoaded() && !_smackerDecoder->endOfVideo())
		frameDelay = _smackerDecoder->getTimeToNextFrame();
	int32 waitTicks = frameDelay - (_vm->_system->getMillis() - _ticks);
	return _vm->_system->getMillis() + waitTicks;
}

void Screen::saveParams() {
	_savedSmackerDecoder = _smackerDecoder;
	_savedFrameDelay = _frameDelay;
	_savedYOffset = _yOffset;
}

void Screen::restoreParams() {
	_smackerDecoder = _savedSmackerDecoder;
	_frameDelay = _savedFrameDelay;
	_yOffset = _savedYOffset;
}

void Screen::setFps(int fps) {
	_frameDelay = 1000 / fps;
}

int Screen::getFps() {
	return 1000 / _frameDelay;
}

void Screen::setYOffset(int16 yOffset) {
	_yOffset = yOffset;
}

int16 Screen::getYOffset() {
	return _yOffset;
}

void Screen::setPaletteData(byte *paletteData) {
	_paletteChanged = true;
	_paletteData = paletteData;
	_hdFade = HdFade(); // HD fade state belongs to the previously shown palette
}

void Screen::unsetPaletteData(byte *paletteData) {
	if (_paletteData == paletteData) {
		_paletteChanged = false;
		_paletteData = nullptr;
	}
}

void Screen::testPalette(byte *paletteData) {
	if (_paletteData == paletteData)
		_paletteChanged = true;
}

void Screen::updatePalette() {
	if (_paletteChanged && _paletteData && _hdScreen) {
		updateHdPalette();
		_paletteChanged = false;
		return;
	}
	if (_paletteChanged && _paletteData) {
		byte *tempPalette = new byte[768];
		for (int i = 0; i < 256; i++) {
			tempPalette[i * 3 + 0] = _paletteData[i * 4 + 0];
			tempPalette[i * 3 + 1] = _paletteData[i * 4 + 1];
			tempPalette[i * 3 + 2] = _paletteData[i * 4 + 2];
		}
		_vm->_system->getPaletteManager()->setPalette(tempPalette, 0, 256);
		delete[] tempPalette;
		_paletteChanged = false;
	}
}

void Screen::clear() {
	memset(_backScreen->getPixels(), 0, _backScreen->pitch * _backScreen->h);
	_fullRefresh = true;
	clearRenderQueue();
}

void Screen::clearRenderQueue() {
	_renderQueue->clear();
	_prevRenderQueue->clear();
}

void Screen::drawSurface2(const Graphics::Surface *surface, NDrawRect &drawRect, NRect &clipRect, bool transparent, byte version,
			  const Graphics::Surface *shadowSurface, byte alphaColor) {

	int16 destX, destY;
	NRect ddRect;

	if (drawRect.x + drawRect.width >= clipRect.x2)
		ddRect.x2 = clipRect.x2 - drawRect.x;
	else
		ddRect.x2 = drawRect.width;

	if (drawRect.x < clipRect.x1) {
		destX = clipRect.x1;
		ddRect.x1 = clipRect.x1 - drawRect.x;
	} else {
		destX = drawRect.x;
		ddRect.x1 = 0;
	}

	if (drawRect.y + drawRect.height >= clipRect.y2)
		ddRect.y2 = clipRect.y2 - drawRect.y;
	else
		ddRect.y2 = drawRect.height;

	if (drawRect.y < clipRect.y1) {
		destY = clipRect.y1;
		ddRect.y1 = clipRect.y1 - drawRect.y;
	} else {
		destY = drawRect.y;
		ddRect.y1 = 0;
	}

	queueBlit(surface, destX, destY, ddRect, transparent, version, shadowSurface, alphaColor);

}

void Screen::drawSurface3(const Graphics::Surface *surface, int16 x, int16 y, NDrawRect &drawRect, NRect &clipRect, bool transparent, byte version) {

	int16 destX, destY;
	NRect ddRect;

	if (x + drawRect.width >= clipRect.x2)
		ddRect.x2 = clipRect.x2 - drawRect.x - x;
	else
		ddRect.x2 = drawRect.x + drawRect.width;

	if (x < clipRect.x1) {
		destX = clipRect.x1;
		ddRect.x1 = clipRect.x1 + drawRect.x - x;
	} else {
		destX = x;
		ddRect.x1 = drawRect.x;
	}

	if (y + drawRect.height >= clipRect.y2)
		ddRect.y2 = clipRect.y2 + drawRect.y - y;
	else
		ddRect.y2 = drawRect.y + drawRect.height;

	if (y < clipRect.y1) {
		destY = clipRect.y1;
		ddRect.y1 = clipRect.y1 + drawRect.y - y;
	} else {
		destY = y;
		ddRect.y1 = drawRect.y;
	}

	queueBlit(surface, destX, destY, ddRect, transparent, version);

}

void Screen::drawDoubleSurface2(const Graphics::Surface *surface, NDrawRect &drawRect) {

	const byte *source = (const byte*)surface->getPixels();
	byte *dest = (byte*)_backScreen->getBasePtr(drawRect.x, drawRect.y);

	for (int16 yc = 0; yc < surface->h; yc++) {
		byte *row = dest;
		for (int16 xc = 0; xc < surface->w; xc++) {
			*row++ = *source;
			*row++ = *source++;
		}
		memcpy(dest + _backScreen->pitch, dest, surface->w * 2);
		dest += _backScreen->pitch;
		dest += _backScreen->pitch;
	}

	_fullRefresh = true; // See Screen::update

}

void Screen::drawDoubleSurface2Alpha(const Graphics::Surface *surface, NDrawRect &drawRect, byte alphaColor) {

	const byte *source = (const byte*)surface->getPixels();
	byte *dest = (byte*)_backScreen->getBasePtr(drawRect.x, drawRect.y);

	for (int16 yc = 0; yc < surface->h; yc++) {
		byte *row = dest;
		for (int16 xc = 0; xc < surface->w; xc++) {
			if (*source != alphaColor) {
				row[0] = *source;
				row[1] = *source;
				row[_backScreen->pitch] = *source;
				row[_backScreen->pitch + 1] = *source;
			}
			source++;
			row += 2;
		}
		dest += _backScreen->pitch;
		dest += _backScreen->pitch;
	}

	_fullRefresh = true; // See Screen::update

}

void Screen::drawUnk(const Graphics::Surface *surface, NDrawRect &drawRect, NDrawRect &sysRect, NRect &clipRect, bool transparent, byte version) {

	int16 x, y;
	bool xflag, yflag;
	NDrawRect newDrawRect;

	x = sysRect.x;
	if (sysRect.width <= x || -sysRect.width >= x)
		x = x % sysRect.width;
	if (x < 0)
		x += sysRect.width;

	y = sysRect.y;
	if (y >= sysRect.height || -sysRect.height >= y)
		y = y % sysRect.height;
	if (y < 0)
		y += sysRect.height;

	xflag = x <= 0;
	yflag = y <= 0;

	newDrawRect.x = x;
	newDrawRect.width = sysRect.width - x;
	if (drawRect.width < newDrawRect.width) {
		xflag = true;
		newDrawRect.width = drawRect.width;
	}

	newDrawRect.y = y;
	newDrawRect.height = sysRect.height - y;
	if (drawRect.height < newDrawRect.height) {
		yflag = true;
		newDrawRect.height = drawRect.height;
	}

	drawSurface3(surface, drawRect.x, drawRect.y, newDrawRect, clipRect, transparent, version);

	if (!xflag) {
		newDrawRect.x = 0;
		newDrawRect.y = y;
		newDrawRect.width = x + drawRect.width - sysRect.width;
		newDrawRect.height = sysRect.height - y;
		if (drawRect.height < newDrawRect.height)
			newDrawRect.height = drawRect.height;
		drawSurface3(surface, sysRect.width + drawRect.x - x, drawRect.y, newDrawRect, clipRect, transparent, version);
	}

	if (!yflag) {
		newDrawRect.x = x;
		newDrawRect.y = 0;
		newDrawRect.width = sysRect.width - x;
		newDrawRect.height = y + drawRect.height - sysRect.height;
		if (drawRect.width < newDrawRect.width)
			newDrawRect.width = drawRect.width;
		drawSurface3(surface, drawRect.x, sysRect.height + drawRect.y - y, newDrawRect, clipRect, transparent, version);
	}

	if (!xflag && !yflag) {
		newDrawRect.x = 0;
		newDrawRect.y = 0;
		newDrawRect.width = x + drawRect.width - sysRect.width;
		newDrawRect.height = y + drawRect.height - sysRect.height;
		drawSurface3(surface, sysRect.width + drawRect.x - x, sysRect.height + drawRect.y - y, newDrawRect, clipRect, transparent, version);
	}

}

void Screen::drawSurfaceClipRects(const Graphics::Surface *surface, NDrawRect &drawRect, NRect *clipRects, uint clipRectsCount, bool transparent, byte version) {
	NDrawRect clipDrawRect(0, 0, drawRect.width, drawRect.height);
	for (uint i = 0; i < clipRectsCount; i++)
		drawSurface3(surface, drawRect.x, drawRect.y, clipDrawRect, clipRects[i], transparent, version);
}

void Screen::queueBlit(const Graphics::Surface *surface, int16 destX, int16 destY, NRect &ddRect, bool transparent, byte version,
		       const Graphics::Surface *shadowSurface, byte alphaColor) {

	const int width = ddRect.x2 - ddRect.x1;
	const int height = ddRect.y2 - ddRect.y1;

	if (width <= 0 || height <= 0)
		return;

	RenderItem renderItem;
	renderItem._surface = surface;
	renderItem._shadowSurface = shadowSurface;
	renderItem._destX = destX;
	renderItem._destY = destY;
	renderItem._srcX = ddRect.x1;
	renderItem._srcY = ddRect.y1;
	renderItem._width = width;
	renderItem._height = height;
	renderItem._transparent = transparent;
	renderItem._version = version;
	renderItem._alphaColor = alphaColor;
	_renderQueue->push_back(renderItem);

}

void Screen::blitRenderItem(const RenderItem &renderItem, const Common::Rect &clipRect) {

	const Graphics::Surface *surface = renderItem._surface;
	const Graphics::Surface *shadowSurface = renderItem._shadowSurface;
	const int16 x0 = MAX<int16>(clipRect.left, renderItem._destX);
	const int16 y0 = MAX<int16>(clipRect.top, renderItem._destY);
	const int16 x1 = MIN<int16>(clipRect.right, renderItem._destX + renderItem._width);
	const int16 y1 = MIN<int16>(clipRect.bottom, renderItem._destY + renderItem._height);
	const int16 width = x1 - x0;
	int16 height = y1 - y0;

	if (width < 0 || height < 0)
		return;

	const byte *source = (const byte*)surface->getBasePtr(renderItem._srcX + x0 - renderItem._destX, renderItem._srcY + y0 - renderItem._destY);
	byte *dest = (byte*)_backScreen->getBasePtr(x0, y0);

	if (shadowSurface) {
		const byte *shadowSource = (const byte*)shadowSurface->getBasePtr(x0, y0);
		while (height--) {
			for (int xc = 0; xc < width; xc++)
				if (source[xc] != 0)
					dest[xc] = shadowSource[xc];
			source += surface->pitch;
			shadowSource += shadowSurface->pitch;
			dest += _backScreen->pitch;
		}
	} else if (!renderItem._transparent) {
		while (height--) {
			memcpy(dest, source, width);
			source += surface->pitch;
			dest += _backScreen->pitch;
		}
	} else if (renderItem._alphaColor == 0) {
		while (height--) {
			for (int xc = 0; xc < width; xc++)
				if (source[xc] != 0)
					dest[xc] = source[xc];
			source += surface->pitch;
			dest += _backScreen->pitch;
		}
	} else {
		while (height--) {
			for (int xc = 0; xc < width; xc++)
				if (source[xc] != renderItem._alphaColor)
					dest[xc] = source[xc];
			source += surface->pitch;
			dest += _backScreen->pitch;
		}
	}

	if (_hdScreen)
		blitRenderItemHd(renderItem, clipRect);

}

// Neverhood Reklayed HD path
//
// The CLUT8 _backScreen is always composited exactly as upstream does. When HD
// is enabled, every blit is additionally mirrored into _hdScreen, a true-color
// buffer at kHdScale x the 640x480 canvas, in the same render-queue order:
// - items without an HD binding: the CLUT8 pixels they just wrote to
//   _backScreen, through the current palette, pixel-doubled;
// - items whose surface has an HD binding: pixels sampled from the override,
//   alpha-blended onto whatever is already in _hdScreen.
// All game-side coordinates stay in 640x480 space; scaling happens only here.

void Screen::enableHd(const Graphics::PixelFormat &format) {
	if (_hdScreen)
		return;
	_hdScreen = new Graphics::Surface();
	_hdScreen->create(640 * kHdScale, 480 * kHdScale, format);
	_hdPaletteDirty = true;
	_fullRefresh = true;
}

void Screen::setHdBinding(const Graphics::Surface *surface, const Graphics::Surface *hdSurface, int16 width, int16 height, bool flipX, bool flipY) {
	HdBinding binding;
	binding.hdSurface = hdSurface;
	binding.width = width;
	binding.height = height;
	binding.flipX = flipX;
	binding.flipY = flipY;
	_hdBindings[surface] = binding;
	debug(3, "Screen::setHdBinding() original %dx%d -> HD %dx%d%s%s", width, height, hdSurface->w, hdSurface->h,
		flipX ? " flipX" : "", flipY ? " flipY" : "");
}

void Screen::clearHdBinding(const Graphics::Surface *surface) {
	_hdBindings.erase(surface);
}

void Screen::updateHdPalette() {
	byte tempPalette[768];
	for (int i = 0; i < 256; i++) {
		tempPalette[i * 3 + 0] = _paletteData[i * 4 + 0];
		tempPalette[i * 3 + 1] = _paletteData[i * 4 + 1];
		tempPalette[i * 3 + 2] = _paletteData[i * 4 + 2];
		_hdPalette[i] = _hdScreen->format.RGBToColor(tempPalette[i * 3 + 0], tempPalette[i * 3 + 1], tempPalette[i * 3 + 2]);
	}
	// A true-color game screen has no hardware palette; the CLUT8 mouse cursor
	// gets its colors from the cursor palette instead.
	CursorMan.replaceCursorPalette(tempPalette, 0, 256);
	_hdPaletteDirty = true;
}

void Screen::putHdPixel(int x, int y, uint32 color) {
	byte *dest = (byte*)_hdScreen->getBasePtr(x, y);
	if (_hdScreen->format.bytesPerPixel == 4)
		*(uint32*)dest = color;
	else
		*(uint16*)dest = (uint16)color;
}

void Screen::convertBackScreenToHd(const Common::Rect &rect) {
	for (int y = rect.top; y < rect.bottom; y++) {
		const byte *source = (const byte*)_backScreen->getBasePtr(rect.left, y);
		for (int x = rect.left; x < rect.right; x++) {
			const uint32 color = _hdPalette[*source++];
			for (int sy = 0; sy < kHdScale; sy++)
				for (int sx = 0; sx < kHdScale; sx++)
					putHdPixel(x * kHdScale + sx, y * kHdScale + sy, color);
		}
	}
}

void Screen::copyHdRectToScreen(const Common::Rect &rect) {
	_vm->_system->copyRectToScreen((const byte*)_hdScreen->getBasePtr(rect.left * kHdScale, rect.top * kHdScale), _hdScreen->pitch,
		rect.left * kHdScale, rect.top * kHdScale, rect.width() * kHdScale, rect.height() * kHdScale);
}

static byte hdFadeChannel(byte from, byte to, int amount) {
	const int delta = CLIP<int>(to - from, -amount, amount);
	return (byte)(from + delta);
}

uint32 Screen::applyHdFade(byte r, byte g, byte b) const {
	switch (_hdFade.mode) {
	case HdFade::kToColor:
		r = hdFadeChannel(r, _hdFade.r, _hdFade.amount);
		g = hdFadeChannel(g, _hdFade.g, _hdFade.amount);
		b = hdFadeChannel(b, _hdFade.b, _hdFade.amount);
		break;
	case HdFade::kFromColor:
		r = hdFadeChannel(_hdFade.r, r, _hdFade.amount);
		g = hdFadeChannel(_hdFade.g, g, _hdFade.amount);
		b = hdFadeChannel(_hdFade.b, b, _hdFade.amount);
		break;
	default:
		break;
	}
	return _hdScreen->format.RGBToColor(r, g, b);
}

void Screen::blitRenderItemHd(const RenderItem &renderItem, const Common::Rect &clipRect) {
	const int x0 = MAX<int>(clipRect.left, renderItem._destX);
	const int y0 = MAX<int>(clipRect.top, renderItem._destY);
	const int x1 = MIN<int>(clipRect.right, renderItem._destX + renderItem._width);
	const int y1 = MIN<int>(clipRect.bottom, renderItem._destY + renderItem._height);
	if (x1 <= x0 || y1 <= y0)
		return;

	const Graphics::Surface *surface = renderItem._surface;
	// Surface coordinate = screen coordinate + offset (same mapping as blitRenderItem)
	const int offX = renderItem._srcX - renderItem._destX;
	const int offY = renderItem._srcY - renderItem._destY;
	const byte alphaColor = renderItem._shadowSurface ? 0 : renderItem._alphaColor;
	const bool opaque = !renderItem._transparent && !renderItem._shadowSurface;

	// Shadow items take their pixels from another surface; keep them on the palette path.
	const HdBinding *binding = nullptr;
	if (!renderItem._shadowSurface) {
		Common::HashMap<const Graphics::Surface*, HdBinding>::const_iterator it = _hdBindings.find(surface);
		if (it != _hdBindings.end())
			binding = &it->_value;
	}

	const Graphics::PixelFormat &hdFormat = _hdScreen->format;

	for (int y = y0; y < y1; y++) {
		const byte *source = (const byte*)surface->getBasePtr(x0 + offX, y + offY);
		const byte *back = (const byte*)_backScreen->getBasePtr(x0, y);
		for (int x = x0; x < x1; x++, source++, back++) {
			const bool written = opaque || *source != alphaColor; // same test as the CLUT8 blit
			const uint32 backColor = _hdPalette[*back];
			const int sx = x + offX, sy = y + offY;

			if (!binding || sx >= binding->width || sy >= binding->height) {
				if (written)
					for (int dy = 0; dy < kHdScale; dy++)
						for (int dx = 0; dx < kHdScale; dx++)
							putHdPixel(x * kHdScale + dx, y * kHdScale + dy, backColor);
				continue;
			}

			const Graphics::Surface *hd = binding->hdSurface;
			for (int dy = 0; dy < kHdScale; dy++) {
				int hy = (sy * kHdScale + dy) * hd->h / (binding->height * kHdScale);
				if (binding->flipY)
					hy = hd->h - 1 - hy;
				for (int dx = 0; dx < kHdScale; dx++) {
					int hx = (sx * kHdScale + dx) * hd->w / (binding->width * kHdScale);
					if (binding->flipX)
						hx = hd->w - 1 - hx;
					byte a, r, g, b;
					hd->format.colorToARGB(*(const uint32*)hd->getBasePtr(hx, hy), a, r, g, b);
					const int destX = x * kHdScale + dx, destY = y * kHdScale + dy;
					if (a == 0) {
						// Fully transparent HD pixel: an opaque item still covers what is below
						if (opaque)
							putHdPixel(destX, destY, backColor);
						continue;
					}
					uint32 color = applyHdFade(r, g, b);
					if (a != 255) {
						uint32 under;
						if (opaque) {
							under = backColor;
						} else {
							const byte *p = (const byte*)_hdScreen->getBasePtr(destX, destY);
							under = hdFormat.bytesPerPixel == 4 ? *(const uint32*)p : *(const uint16*)p;
						}
						byte fr, fg, fb, ur, ug, ub;
						hdFormat.colorToRGB(color, fr, fg, fb);
						hdFormat.colorToRGB(under, ur, ug, ub);
						color = hdFormat.RGBToColor((fr * a + ur * (255 - a)) / 255, (fg * a + ug * (255 - a)) / 255, (fb * a + ub * (255 - a)) / 255);
					}
					putHdPixel(destX, destY, color);
				}
			}
		}
	}
}

// Whole-palette fades, mirrored onto HD pixels with the same per-step
// arithmetic Palette::fadeColor() applies to palette entries.

void Screen::hdFadeOutStep(const byte *paletteData, byte r, byte g, byte b, int step) {
	if (!_hdScreen || paletteData != _paletteData)
		return;
	if (_hdFade.mode != HdFade::kToColor || _hdFade.r != r || _hdFade.g != g || _hdFade.b != b) {
		_hdFade.mode = HdFade::kToColor;
		_hdFade.r = r;
		_hdFade.g = g;
		_hdFade.b = b;
		_hdFade.amount = 0;
	}
	_hdFade.amount = MIN(255, _hdFade.amount + step);
	_hdPaletteDirty = true;
}

void Screen::hdFadeInStart(const byte *paletteData, bool fromUniformColor, byte r, byte g, byte b) {
	if (!_hdScreen || paletteData != _paletteData)
		return;
	if (fromUniformColor) {
		// e.g. a black palette fading in: HD pixels start fully at that color
		_hdFade.mode = HdFade::kFromColor;
		_hdFade.r = r;
		_hdFade.g = g;
		_hdFade.b = b;
		_hdFade.amount = 0;
	} else if (_hdFade.mode == HdFade::kToColor && _hdFade.amount >= 255) {
		_hdFade.mode = HdFade::kFromColor;
		_hdFade.amount = 0;
	}
	// Otherwise (partial palette crossfades such as lighting changes) HD pixels
	// are left as they are -- they have no palette indices to crossfade.
	_hdPaletteDirty = true;
}

void Screen::hdFadeInStep(const byte *paletteData, int step) {
	if (!_hdScreen || paletteData != _paletteData)
		return;
	if (_hdFade.mode == HdFade::kFromColor) {
		_hdFade.amount = MIN(255, _hdFade.amount + step);
	} else if (_hdFade.mode == HdFade::kToColor) {
		_hdFade.amount = MAX(0, _hdFade.amount - step);
		if (_hdFade.amount == 0)
			_hdFade.mode = HdFade::kNone;
	}
	_hdPaletteDirty = true;
}

void Screen::hdFadeInDone(const byte *paletteData) {
	if (!_hdScreen || paletteData != _paletteData || _hdFade.mode == HdFade::kNone)
		return;
	_hdFade = HdFade();
	_hdPaletteDirty = true;
}

} // End of namespace Neverhood
