/* Copyright (c) 2013-2019 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#include <mgba-util/vfs.h>
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/core/version.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/input.h>

#include "platform/sdl/sdl-audio.h"
#include "platform/sdl/sdl-events.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_keyboard.h>
#include <emscripten.h>

// global renderer
static struct mEmscriptenRenderer renderer = {
	.audio = { .sampleRate = 48000, .samples = 4096, .fpsTarget = 60.0 },
	.renderFirstFrame = true,
	.fastForwardSpeed = 1,
};

// log utilities
static void _log(struct mLogger*, int category, enum mLogLevel level, const char* format, va_list args);
static struct mLogger logCtx = { .log = _log };

// keypress utilities
static void handleKeypressCore(const struct SDL_KeyboardEvent* event) {
	if (event->keysym.sym == SDLK_f) {
		renderer.fastForwardSpeed = event->type == SDL_KEYDOWN ? 2 : 1;
		return;
	}
	int key = -1;
	if (!(event->keysym.mod & ~(KMOD_NUM | KMOD_CAPS))) {
		key = mInputMapKey(&renderer.core->inputMap, SDL_BINDING_KEY, event->keysym.sym);
	}
	if (key != -1) {
		if (event->type == SDL_KEYDOWN) {
			renderer.core->addKeys(renderer.core, 1 << key);
		} else {
			renderer.core->clearKeys(renderer.core, 1 << key);
		}
	}
}

// emscripten main run loop
void runLoop() {
	union SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if (renderer.core) {
				handleKeypressCore(&event.key);
			}
			break;
		};
	}
	if (renderer.core) {
		double now = emscripten_get_now();
		double elapsed = now - renderer.lastLoopTime;
		renderer.lastLoopTime = now;

		double targetFrameTime = 1000.0 / 60.0;

		renderer.frameTime += elapsed;

		double numberOfFrames = (renderer.frameTime + 0.2) / targetFrameTime;
		Sint32 nowFramesInt = numberOfFrames;
		if (nowFramesInt < 1) {
			return;
		}

		renderer.frameTime -= nowFramesInt * targetFrameTime;

		if (renderer.frameTime < 0.0) {
			renderer.frameTime = 0.0;
		}

		if (renderer.fastForwardSpeed > 1)
			nowFramesInt *= renderer.fastForwardSpeed;

		if (nowFramesInt > 20)
			nowFramesInt = 20;

		if (renderer.renderFirstFrame) {
			renderer.renderFirstFrame = false;
			renderer.core->runFrame(renderer.core);
		} else {
			for (Uint32 i = 0; i < nowFramesInt; i++) {
				renderer.core->runFrame(renderer.core);
			}
		}

		unsigned w, h;
		renderer.core->currentVideoSize(renderer.core, &w, &h);

		SDL_Rect rect = { .x = 0, .y = 0, .w = w, .h = h };
		SDL_UnlockTexture(renderer.sdlTex);
		SDL_RenderCopy(renderer.sdlRenderer, renderer.sdlTex, &rect, &rect);
		SDL_RenderPresent(renderer.sdlRenderer);

		int stride;
		SDL_LockTexture(renderer.sdlTex, 0, (void**) &renderer.outputBuffer, &stride);
		renderer.core->setVideoBuffer(renderer.core, renderer.outputBuffer, stride / BYTES_PER_PIXEL);
		return;
	} else {
		// dont run the main loop if there is no core,  we don't
		// want to handle events unless the core is running for now
		renderer.renderFirstFrame = true;
		emscripten_pause_main_loop();
	}
}

/**
 * Exposed core contract methods
 */

EMSCRIPTEN_KEEPALIVE bool screenshot(char* fileName) {
	bool success = false;
	int mode = O_CREAT | O_TRUNC | O_WRONLY;
	struct VFile* vf;

	if (!renderer.core)
		return false;

	struct VDir* dir = renderer.core->dirs.screenshot;

	if (strlen(fileName)) {
		vf = dir->openFile(dir, fileName, mode);
	} else {
		vf = VDirFindNextAvailable(dir, renderer.core->dirs.baseName, "-", ".png", mode);
	}

	if (!vf)
		return false;

	success = mCoreTakeScreenshotVF(renderer.core, vf);
	vf->close(vf);

	return success;
}

EMSCRIPTEN_KEEPALIVE void buttonPress(int id) {
	if (renderer.core) {
		renderer.core->addKeys(renderer.core, 1 << id);
	}
}

EMSCRIPTEN_KEEPALIVE void buttonUnpress(int id) {
	if (renderer.core) {
		renderer.core->clearKeys(renderer.core, 1 << id);
	}
}

EMSCRIPTEN_KEEPALIVE void setVolume(float vol) {
	if (vol > 2.0 || vol < 0)
		return; // this is a percentage so more than 200% is insane.

	int volume = (int) (vol * 0x100);
	if (renderer.core) {
		if (volume == 0)
			mSDLPauseAudio(&renderer.audio);
		else {
			mSDLResumeAudio(&renderer.audio);
		}
		mCoreConfigSetDefaultIntValue(&renderer.core->config, "volume", volume);
		renderer.core->reloadConfigOption(renderer.core, "volume", &renderer.core->config);
	}
}

EMSCRIPTEN_KEEPALIVE float getVolume() {
	return (float) renderer.core->opts.volume / 0x100;
}

EMSCRIPTEN_KEEPALIVE int getMainLoopTimingMode() {
	int mode = -1;
	int value = -1;
	emscripten_get_main_loop_timing(&mode, &value);
	return mode;
}

EMSCRIPTEN_KEEPALIVE int getMainLoopTimingValue() {
	int mode = -1;
	int value = -1;
	emscripten_get_main_loop_timing(&mode, &value);
	return value;
}

EMSCRIPTEN_KEEPALIVE void setMainLoopTiming(int mode, int value) {
	emscripten_set_main_loop_timing(mode, value);
}

EMSCRIPTEN_KEEPALIVE void setFastForwardMultiplier(int multiplier) {
	if (multiplier > 0) {
		renderer.fastForwardSpeed = multiplier;
		renderer.audio.fpsTarget = (double) 60 * multiplier;

		// fast forward starts at 1, frameskip starts at 0
		mCoreConfigSetDefaultIntValue(&renderer.core->config, "frameskip", multiplier - 1);
		renderer.core->reloadConfigOption(renderer.core, "frameskip", &renderer.core->config);
	}
}

EMSCRIPTEN_KEEPALIVE int getFastForwardMultiplier() {
	return renderer.fastForwardSpeed;
}

EMSCRIPTEN_KEEPALIVE void quitGame() {
	if (renderer.core) {
		renderer.renderFirstFrame = true;
		mSDLPauseAudio(&renderer.audio);
		emscripten_pause_main_loop();
		renderer.core->deinit(renderer.core);
		renderer.core = NULL;
	}
}

EMSCRIPTEN_KEEPALIVE void quitMgba() {
	exit(0);
}

EMSCRIPTEN_KEEPALIVE void quickReload() {
	renderer.renderFirstFrame = true;
	renderer.core->reset(renderer.core);
}

EMSCRIPTEN_KEEPALIVE void pauseGame() {
	renderer.renderFirstFrame = true;
	mSDLPauseAudio(&renderer.audio);
	emscripten_pause_main_loop();
}

EMSCRIPTEN_KEEPALIVE void resumeGame() {
	int vol = -1;

	mCoreConfigGetIntValue(&renderer.core->config, "volume", &vol);

	if (vol > 0) {
		mSDLResumeAudio(&renderer.audio);
	}
	emscripten_resume_main_loop();
}

EMSCRIPTEN_KEEPALIVE void setEventEnable(bool toggle) {
	int state = toggle ? SDL_ENABLE : SDL_DISABLE;
	SDL_EventState(SDL_TEXTINPUT, state);
	SDL_EventState(SDL_KEYDOWN, state);
	SDL_EventState(SDL_KEYUP, state);
	SDL_EventState(SDL_MOUSEMOTION, state);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, state);
	SDL_EventState(SDL_MOUSEBUTTONUP, state);
}

// bindingName is the key name of what you want to bind to an input
// inputCode is the code of the key input, see keyBindings in pre.js
// this should work for a good variety of keys, but not all are supported yet
EMSCRIPTEN_KEEPALIVE void bindKey(char* bindingName, int inputCode) {
	int bindingSDLKeyCode = SDL_GetKeyFromName(bindingName);
	mInputBindKey(&renderer.core->inputMap, SDL_BINDING_KEY, bindingSDLKeyCode, inputCode);
}

EMSCRIPTEN_KEEPALIVE bool saveState(int slot) {
	if (renderer.core) {
		return mCoreSaveState(renderer.core, slot, SAVESTATE_ALL);
	}
	return false;
}

EMSCRIPTEN_KEEPALIVE bool loadState(int slot) {
	if (renderer.core) {
		return mCoreLoadState(renderer.core, slot, SAVESTATE_ALL);
	}
	return false;
}

// loads all cheats files located in the cores cheatsPath,
// cheat files must match the name of the rom they are
// to be applied to, and must end with the extension .cheats
// supported cheat formats:
//  - mGBA custom format
//  - libretro format
//  - EZFCht format
EMSCRIPTEN_KEEPALIVE bool autoLoadCheats() {
	return mCoreAutoloadCheats(renderer.core);
}

EMSCRIPTEN_KEEPALIVE bool loadGame(const char* name) {
	if (renderer.core) {
		renderer.core->deinit(renderer.core);
		renderer.core = NULL;
	}
	renderer.core = mCoreFind(name);
	if (!renderer.core) {
		return false;
	}
	renderer.core->init(renderer.core);
	renderer.core->opts.savegamePath = strdup("/data/saves");
	renderer.core->opts.savestatePath = strdup("/data/states");
	renderer.core->opts.cheatsPath = strdup("/data/cheats");
	renderer.core->opts.screenshotPath = strdup("/data/screenshots");

	mCoreLoadFile(renderer.core, name);
	mCoreConfigInit(&renderer.core->config, "wasm");
	mCoreConfigSetDefaultValue(&renderer.core->config, "idleOptimization", "detect");
	mCoreConfigSetDefaultIntValue(&renderer.core->config, "volume", 0x100);
	mInputMapInit(&renderer.core->inputMap, &GBAInputInfo);
	mDirectorySetMapOptions(&renderer.core->dirs, &renderer.core->opts);
	mCoreAutoloadSave(renderer.core);
	mCoreAutoloadCheats(renderer.core);
	mSDLInitBindingsGBA(&renderer.core->inputMap);

	unsigned w, h;
	renderer.core->baseVideoSize(renderer.core, &w, &h);
	if (renderer.sdlTex) {
		SDL_DestroyTexture(renderer.sdlTex);
	}
	renderer.sdlTex =
	    SDL_CreateTexture(renderer.sdlRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);

	int stride;
	SDL_LockTexture(renderer.sdlTex, 0, (void**) &renderer.outputBuffer, &stride);
	renderer.core->setVideoBuffer(renderer.core, renderer.outputBuffer, stride / BYTES_PER_PIXEL);
	renderer.core->setAudioBufferSize(renderer.core, renderer.audio.samples * 2);

	renderer.core->reset(renderer.core);

	renderer.core->currentVideoSize(renderer.core, &w, &h);
	SDL_SetWindowSize(renderer.window, w, h);
	EM_ASM(
	    {
		    Module.canvas.width = $0;
		    Module.canvas.height = $1;
	    },
	    w, h);

	renderer.audio.core = renderer.core;
	mSDLResumeAudio(&renderer.audio);
	emscripten_resume_main_loop();
	return true;
}

EMSCRIPTEN_KEEPALIVE bool saveStateSlot(int slot, int flags) {
	if (!renderer.core) {
		return false;
	}
	return mCoreSaveState(renderer.core, slot, flags);
}

EMSCRIPTEN_KEEPALIVE bool loadStateSlot(int slot, int flags) {
	if (!renderer.core) {
		return false;
	}
	return mCoreLoadState(renderer.core, slot, flags);
}

// addCoreCallbacks clears core callbacks, and registers new callbacks with the core
// Note: function pointers from javascript are expected to be kept alive until no longer needed,
//       it is the responsibility of the javascript code to ensure the function references are kept alive
EMSCRIPTEN_KEEPALIVE void
addCoreCallbacks(void (*alarmCallbackPtr)(void* context), void (*coreCrashedCallbackPtr)(void* context),
                 void (*keysReadCallbackPtr)(void* context), void (*saveDataUpdatedCallbackPtr)(void* context),
                 // void (*shutdownCallbackPtr)(void* context), void (*sleepCallbackPtr)(void* context),
                 void (*videoFrameEndedCallbackPtr)(void* context),
                 void (*videoFrameStartedCallbackPtr)(void* context)) {
	if (renderer.core) {
		struct mCoreCallbacks callbacks = {};
		renderer.core->clearCoreCallbacks(renderer.core);

		if (alarmCallbackPtr)
			callbacks.alarm = alarmCallbackPtr;

		if (coreCrashedCallbackPtr)
			callbacks.coreCrashed = coreCrashedCallbackPtr;

		if (keysReadCallbackPtr)
			callbacks.keysRead = keysReadCallbackPtr;

		if (saveDataUpdatedCallbackPtr)
			callbacks.savedataUpdated = saveDataUpdatedCallbackPtr;

		// NOTE: I do not believe these behaviors are currently accessible to be called from the wasm interface
		// if (shutdownCallbackPtr)
		// 	callbacks.shutdown = shutdownCallbackPtr;

		// if (sleepCallbackPtr)
		// 	callbacks.sleep = sleepCallbackPtr;

		if (videoFrameEndedCallbackPtr)
			callbacks.videoFrameEnded = videoFrameEndedCallbackPtr;

		if (videoFrameStartedCallbackPtr)
			callbacks.videoFrameStarted = videoFrameStartedCallbackPtr;

		renderer.core->addCoreCallbacks(renderer.core, &callbacks);
	}
}

void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}

EMSCRIPTEN_KEEPALIVE void setupConstants(void) {
	EM_ASM(({
		       Module.version = {
			       gitCommit : UTF8ToString($0),
			       gitShort : UTF8ToString($1),
			       gitBranch : UTF8ToString($2),
			       gitRevision : $3,
			       binaryName : UTF8ToString($4),
			       projectName : UTF8ToString($5),
			       projectVersion : UTF8ToString($6)
		       };
	       }),
	       gitCommit, gitCommitShort, gitBranch, gitRevision, binaryName, projectName, projectVersion);
}

CONSTRUCTOR(premain) {
	setupConstants();
}

int excludeKeys(void* userdata, SDL_Event* event) {
	UNUSED(userdata);

	switch (event->key.keysym.sym) {
	case SDLK_TAB: // ignored for a11y during gameplay
	case SDLK_SPACE:
		return 0; // Value will be ignored
	default:
		return 1;
	};
}

int main() {
	mLogSetDefaultLogger(&logCtx);

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS);
	renderer.window = SDL_CreateWindow(NULL, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                                   GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS, SDL_WINDOW_OPENGL);
	renderer.sdlRenderer =
	    SDL_CreateRenderer(renderer.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	mSDLInitAudio(&renderer.audio, NULL);

	// exclude specific key events
	SDL_SetEventFilter(excludeKeys, NULL);

	renderer.lastLoopTime = 0.0;
	renderer.frameTime = 0.0;

	emscripten_set_main_loop(runLoop, 0, 0);
	return 0;
}
