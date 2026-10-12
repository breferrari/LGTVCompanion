#pragma once
#include <Windows.h>
#include <functional>
#include <string>

// Detects Windows being left without any active display path while a monitor is still connected, and has
// the device woken so that Windows can enable it again.
//
// This is the state dxgkrnl reports as VIDEO_DXGKRNL_BLACK_SCREEN_LIVEDUMP (0x1A8), argument 0xA: a mode set
// to enable a connected monitor failed. It happens when the GPU must (re)light a TV that has been in standby
// for a while, e.g. when Windows wakes the displays or a streaming host changes the resolution. Retrying the
// mode set does not help while the TV sleeps; Windows enables the display as soon as the TV wakes. The service
// decides how to wake the device (while streaming: with the screen off). The guard traces every step, and
// reports when the display configuration has settled after a recovery.
class DisplayGuard
{
public:
	struct Hooks
	{
		std::function<void(const std::wstring&)>	trace;
		std::function<void(void)>					display_lost;		// ask the service to wake the device
		std::function<void(void)>					display_settled;	// the recovered configuration is stable
	};

	explicit DisplayGuard(Hooks hooks);

	// Evaluate the display state now. Call from a timer and on display related events.
	void		check(void);
	// A display change or display power on event occurred. Reschedule the timer with checkInterval() after this.
	void		onDisplayEvent(void);
	// Whether a lost display is being recovered or has not settled yet.
	bool		busy(void) const;
	// Delay in ms until the next timer driven check.
	UINT		checkInterval(void) const;

private:
	enum class Phase { Healthy, Lost, Settling };

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
	void			checkHealthy(const Snapshot&, ULONGLONG now);
	void			checkLost(const Snapshot&, ULONGLONG now);
	void			checkSettling(const Snapshot&, ULONGLONG now);
	void			requestWake(ULONGLONG now);
	void			reset(void);

	Hooks			hooks_;
	Phase			phase_ = Phase::Healthy;
	ULONGLONG		suspect_since_ = 0;			// no active path seen, within the grace period
	ULONGLONG		lost_since_ = 0;
	ULONGLONG		last_wake_request_ = 0;
	int				wake_requests_ = 0;
	bool			waiting_for_event_ = false;	// wake requests exhausted
	ULONGLONG		monitor_missing_since_ = 0;	// the monitor vanished while lost (re-enumeration)
	ULONGLONG		restored_at_ = 0;
	ULONGLONG		last_display_event_ = 0;
};
