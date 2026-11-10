// SPDX-License-Identifier: GPL-3.0-or-later
// SLOPPY CAMERA FX by nicktheduke: recorded self test. When Data\SKSE\Plugins\SloppyCameraFX\SelfTest.txt exists, every
// save load plays each lens effect once, in a fixed order, and logs "[selftest] BEGIN/END <name>" for cutting footage.
// Without the file nothing happens.
#pragma once

namespace SloppyCameraFX::SelfTest
{
	void Install();       // main thread, data loaded: weather mode listens for loading screens
	void OnGameLoaded();  // main thread
	void Tick();          // render thread, once per frame
}
