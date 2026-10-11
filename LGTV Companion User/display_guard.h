#pragma once
#include <Windows.h>
#include <functional>
#include <string>

// Recovers from Windows being left without any active display path while a monitor is still connected.
//
// This is the state dxgkrnl reports as VIDEO_DXGKRNL_BLACK_SCREEN_LIVEDUMP (0x1A8), argument 0xA: a mode set
// to enable a connected monitor failed. It happens when the GPU must (re)light a TV that has been in standby
// for a while, e.g. when Windows wakes the displays or a streaming host changes the resolution. Normally the TV
// powering on re-triggers a successful mode set, but while a remote stream keeps the TV off nothing does, and
// Windows never retries by itself. The guard detects the state, retries the mode set with back-off and traces
// every step, including whether and how the display path came back.
class DisplayGuard
{
public:
	using Trace = std::function<void(const std::wstring&)>;

	explicit DisplayGuard(Trace trace);

	// Evaluate the display state now. Call from a timer and on display related events.
	void		check(void);
	// A display, device or power event occurred. Allows a fresh round of attempts if recovery was paused.
	void		rearm(void);
	// Delay in ms until the next timer driven check.
	UINT		checkInterval(void) const;

private:
	struct Snapshot
	{
		bool			valid = false;			// false when the display configuration could not be queried
		UINT32			active_paths = 0;
		UINT32			connected_monitors = 0;
		std::wstring	active_description;		// e.g. "LG TV SSCR2 3840x2160"
		std::wstring	monitor_description;	// e.g. "GSM82CD"
	};

	static Snapshot	takeSnapshot(void);
	static bool		isPhysicalConsoleSession(void);
	void			attemptRecovery(const Snapshot& snapshot);
	void			reset(void);

	Trace			trace_;
	bool			lost_ = false;				// connected monitor(s) but no active path
	bool			paused_ = false;			// attempts exhausted, waiting for an event
	ULONGLONG		lost_since_ = 0;
	ULONGLONG		next_attempt_ = 0;
	ULONGLONG		last_attempt_ = 0;
	LONG			last_attempt_result_ = ERROR_SUCCESS;
	int				attempts_ = 0;
};
