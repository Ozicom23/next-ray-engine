#pragma once

// The Mod Organizer 2 mod a script comes from: MO2's virtual file system shows the game one merged gamedata, so the
// active profile's modlist.txt and the enabled mods' gamedata\scripts folders are read once, at the first question.
// MO2 is found from the game's parent process (MO2 starts it), else at ..\gamma (GAMMA's layout)
namespace mod_origin
{
// "foo" or "foo.script" (any case): the name of the enabled mod with the highest priority that has
// gamedata\scripts\foo.script, nullptr when none (the game's own gamedata or its archives) or MO2 wasn't found
XRCORE_API LPCSTR of_script(LPCSTR script);
// The same without reading the mods: nullptr until of_script read them once (the crash handler asks this)
XRCORE_API LPCSTR of_script_if_known(LPCSTR script);
// The MO2 profile read, "" when none
XRCORE_API LPCSTR profile();
}
