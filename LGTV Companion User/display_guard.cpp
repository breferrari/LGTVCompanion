#include "display_guard.h"
#include <cfgmgr32.h>
#include <initguid.h>
#include <ntddvdeo.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "cfgmgr32.lib")

// Trace text is relayed to the service as a command line, where " -" starts a new command. Keep it out of messages.

namespace
{
	// No active path for this long before the display counts as lost. Lets a mode set already in flight (a
	// resolution change, a monitor waking up) complete on its own.
	constexpr ULONGLONG		kGraceMs = 1000;
	// Repeat the wake request while the display stays lost, up to a limit.
	constexpr ULONGLONG		kWakeRepeatMs = 20000;
	constexpr int			kMaxWakeRequests = 3;
	// The monitor vanishes briefly while the device wakes and re-enumerates. Only this long counts as unplugged.
	constexpr ULONGLONG		kMonitorGoneMs = 60000;
	// A recovered configuration counts as settled after this long without display changes.
	constexpr ULONGLONG		kSettleMs = 10000;
	constexpr UINT			kIntervalBusy = 1000;
	constexpr UINT			kIntervalNormal = 3000;

	std::wstring seconds(ULONGLONG ms)
	{
		wchar_t buffer[32];
		swprintf_s(buffer, L"%.1f s", ms / 1000.0);
		return buffer;
	}

	// "\\?\DISPLAY#GSM82CD#5&3a415d7b&1&UID4352#{...}" -> "GSM82CD"
	std::wstring monitorId(const std::wstring& interface_path)
	{
		size_t begin = interface_path.find(L'#');
		if (begin == std::wstring::npos)
			return interface_path;
		size_t end = interface_path.find(L'#', begin + 1);
		return interface_path.substr(begin + 1, end == std::wstring::npos ? std::wstring::npos : end - begin - 1);
	}

	void append(std::wstring& list, const std::wstring& item)
	{
		if (!list.empty())
			list += L", ";
		list += item;
	}
}

DisplayGuard::DisplayGuard(Hooks hooks) : hooks_(std::move(hooks))
{
}

void DisplayGuard::check(void)
{
	if (!isPhysicalConsoleSession())
		return;
	const Snapshot snapshot = takeSnapshot();
	if (!snapshot.valid)
		return;	// transient (e.g. mid mode set or secure desktop). Try again on the next check
	const ULONGLONG now = GetTickCount64();
	switch (phase_)
	{
	case Phase::Healthy:	checkHealthy(snapshot, now);	break;
	case Phase::Lost:		checkLost(snapshot, now);		break;
	case Phase::Settling:	checkSettling(snapshot, now);	break;
	}
}

void DisplayGuard::checkHealthy(const Snapshot& snapshot, ULONGLONG now)
{
	if (snapshot.active_paths > 0 || snapshot.connected_monitors == 0)	// fine, or headless
	{
		suspect_since_ = 0;
		return;
	}
	if (suspect_since_ == 0)
		suspect_since_ = now;
	if (now - suspect_since_ < kGraceMs)
		return;

	phase_ = Phase::Lost;
	lost_since_ = suspect_since_;
	suspect_since_ = 0;
	hooks_.trace(L"No active display path while " + std::to_wstring(snapshot.connected_monitors) + L" monitor(s) connected ("
		+ snapshot.monitor_description + L"). Windows failed to enable the display");
	requestWake(now);
}

void DisplayGuard::checkLost(const Snapshot& snapshot, ULONGLONG now)
{
	if (snapshot.active_paths > 0)
	{
		std::wstring how = wake_requests_ > 0
			? seconds(now - last_wake_request_) + L" after wake request " + std::to_wstring(wake_requests_)
			: L"on its own";
		hooks_.trace(L"Display path restored after " + seconds(now - lost_since_) + L", " + how + L". Active: " + snapshot.active_description);
		phase_ = Phase::Settling;
		restored_at_ = now;
		monitor_missing_since_ = 0;
		return;
	}

	if (snapshot.connected_monitors == 0)
	{
		if (monitor_missing_since_ == 0)
		{
			monitor_missing_since_ = now;
			hooks_.trace(L"Monitor vanished from Windows after " + seconds(now - lost_since_) + L". Expected while the device wakes and reconnects");
		}
		else if (now - monitor_missing_since_ >= kMonitorGoneMs)
		{
			hooks_.trace(L"Monitor gone for " + seconds(now - monitor_missing_since_) + L". Treating it as unplugged. Recovery stopped");
			reset();
		}
		return;
	}
	monitor_missing_since_ = 0;

	if (waiting_for_event_ || now - last_wake_request_ < kWakeRepeatMs)
		return;
	if (wake_requests_ < kMaxWakeRequests)
		requestWake(now);
	else
	{
		waiting_for_event_ = true;
		hooks_.trace(L"Display path still lost after " + seconds(now - lost_since_) + L" and " + std::to_wstring(wake_requests_)
			+ L" wake requests. Requesting again on the next display change or display power on event");
	}
}

void DisplayGuard::checkSettling(const Snapshot& snapshot, ULONGLONG now)
{
	if (snapshot.active_paths == 0)	// lost again: start over, with the grace period
	{
		reset();
		checkHealthy(snapshot, now);
		return;
	}
	if (now - (std::max)(restored_at_, last_display_event_) < kSettleMs)
		return;
	hooks_.trace(L"Display configuration settled " + seconds(now - restored_at_) + L" after the restore. Active: " + snapshot.active_description);
	reset();
	hooks_.display_settled();
}

void DisplayGuard::requestWake(ULONGLONG now)
{
	wake_requests_++;
	last_wake_request_ = now;
	hooks_.trace(L"Wake request " + std::to_wstring(wake_requests_) + L"/" + std::to_wstring(kMaxWakeRequests) + L" sent to the service");
	hooks_.display_lost();
}

void DisplayGuard::onDisplayEvent(void)
{
	last_display_event_ = GetTickCount64();
	if (phase_ == Phase::Lost && waiting_for_event_)
	{
		waiting_for_event_ = false;
		wake_requests_ = 0;
		last_wake_request_ = 0;	// request again on the next check
	}
}

bool DisplayGuard::busy(void) const
{
	return phase_ != Phase::Healthy || suspect_since_ != 0;
}

UINT DisplayGuard::checkInterval(void) const
{
	return busy() ? kIntervalBusy : kIntervalNormal;
}

void DisplayGuard::reset(void)
{
	phase_ = Phase::Healthy;
	suspect_since_ = 0;
	lost_since_ = 0;
	last_wake_request_ = 0;
	wake_requests_ = 0;
	waiting_for_event_ = false;
	monitor_missing_since_ = 0;
	restored_at_ = 0;
}

DisplayGuard::Snapshot DisplayGuard::takeSnapshot(void)
{
	Snapshot snapshot;

	// Connected monitors: monitor device interfaces which are present, i.e. what dxgkrnl sees as connected
	ULONG length = 0;
	if (CM_Get_Device_Interface_List_SizeW(&length, (LPGUID)&GUID_DEVINTERFACE_MONITOR, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
		return snapshot;
	std::vector<wchar_t> interfaces(length > 0 ? length : 1, L'\0');
	if (length > 1 && CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_MONITOR, NULL, interfaces.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
		return snapshot;	// the list changed between the two calls
	for (const wchar_t* item = interfaces.data(); *item; item += wcslen(item) + 1)
	{
		snapshot.connected_monitors++;
		append(snapshot.monitor_description, monitorId(item));
	}

	// Active paths
	UINT32 path_count = 0;
	UINT32 mode_count = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
		return snapshot;
	if (path_count > 0)
	{
		std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), NULL) != ERROR_SUCCESS)
			return snapshot;
		paths.resize(path_count);
		for (const auto& path : paths)
		{
			std::wstring item = L"unknown monitor";
			DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
			name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
			name.header.size = sizeof(name);
			name.header.adapterId = path.targetInfo.adapterId;
			name.header.id = path.targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS && name.monitorFriendlyDeviceName[0])
				item = name.monitorFriendlyDeviceName;
			const UINT32 mode_index = path.sourceInfo.modeInfoIdx;
			if (mode_index < mode_count && modes[mode_index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
				item += L" " + std::to_wstring(modes[mode_index].sourceMode.width) + L"x" + std::to_wstring(modes[mode_index].sourceMode.height);
			append(snapshot.active_description, item);
		}
	}
	snapshot.active_paths = path_count;
	snapshot.valid = true;
	return snapshot;
}

bool DisplayGuard::isPhysicalConsoleSession(void)
{
	DWORD session_id = 0;
	return ProcessIdToSessionId(GetCurrentProcessId(), &session_id) && session_id == WTSGetActiveConsoleSessionId();
}
