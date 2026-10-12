#pragma once
// Fork specific definitions, see Docs/Fork.md. Included at the end of common_app_define.h and kept in this
// separate file, so that upstream changes to common_app_define.h merge without conflicts.

// FORK_VERSION is passed by Directory.Build.targets, from ForkVersion in Directory.Build.props
#ifndef FORK_VERSION
#error FORK_VERSION is not defined. Build through MSBuild, which reads it from Directory.Build.props
#endif

#undef APP_VERSION
#define APP_VERSION						L"" FORK_VERSION

// Releases and update checks come from the fork
#undef NEWRELEASELINK
#define NEWRELEASELINK					L"https://github.com/breferrari/LGTVCompanion/releases"
#undef VERSIONCHECKLINK
#define VERSIONCHECKLINK				L"https://api.github.com/repos/breferrari/LGTVCompanion/releases/latest"
