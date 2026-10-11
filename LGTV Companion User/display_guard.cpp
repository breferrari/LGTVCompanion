#include "display_guard.h"
#include <cfgmgr32.h>
#include <initguid.h>
#include <ntddvdeo.h>
#include <iterator>
#include <vector>

#pragma comment(lib, "cfgmgr32.lib")

// Trace text is relayed to the service as a command line, where " -" starts a new command. Keep it out of messages.

namespace
{
	// Seconds to wait before each recovery attempt. The first entry is a grace period which lets a mode set
	// already in flight (a resolution change, a monitor waking up) complete on its own.
	constexpr UINT			kAttemptDelays[] = { 2, 3, 5, 10, 15, 30, 60, 60, 120, 120 };
	constexpr int			kMaxAttempts = (int)std::size(kAttemptDelays);
	// How long the last attempt gets to take effect before recovery is paused.
	constexpr ULONGLONG		kSettleMs = 10000;
	// A restore seen this soon after a successful SetDisplayConfig call is credited to that attempt.
	constexpr ULONGLONG		kAttributionWindowMs = 5000;
	constexpr UINT			kIntervalWhileRecovering = 1000;
	constexpr UINT			kIntervalNormal = 3000;

	struct Method
	{
		UINT32			flags;
		const wchar_t*	description;
	};
	// Alternate between re-applying the configuration Windows has saved for the connected monitors, and asking
	// for the extended topology, which makes Windows build and commit a fresh set of paths.
	constexpr Method		kMethods[] = {
		{ SDC_APPLY | SDC_USE_DATABASE_CURRENT,	L"re-apply the saved display configuration" },
		{ SDC_APPLY | SDC_TOPOLOGY_EXTEND,		L"apply the extended topology" },
	};

	std::wstring seconds(ULONGLONG ms)
	{
		wchar_t buffer[32];
		swprintf_s(buffer, L"%.1f s", ms / 1000.0);
		return buffer;
	}

	std::wstring resultText(LONG result)
	{
		switch (result)
		{
		case ERROR_SUCCESS:				return L"ERROR_SUCCESS";
		case ERROR_INVALID_PARAMETER:	return L"ERROR_INVALID_PARAMETER";
		case ERROR_GEN_FAILURE:			return L"ERROR_GEN_FAILURE";
		case ERROR_NOT_SUPPORTED:		return L"ERROR_NOT_SUPPORTED";
		case ERROR_ACCESS_DENIED:		return L"ERROR_ACCESS_DENIED";
		case ERROR_BAD_CONFIGURATION:	return L"ERROR_BAD_CONFIGURATION";
		default:						return L"error " + std::to_wstring(result);
		}
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

DisplayGuard::DisplayGuard(Trace trace) : trace_(std::move(trace))
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

	if (snapshot.active_paths > 0)
	{
		if (lost_)
		{
			std::wstring how;
			if (last_attempt_ != 0 && last_attempt_result_ == ERROR_SUCCESS && now - last_attempt_ <= kAttributionWindowMs)
				how = L"by recovery attempt " + std::to_wstring(attempts_);
			else if (attempts_ > 0)
				how = L"without help from the " + std::to_wstring(attempts_) + L" recovery attempt(s)";
			else
				how = L"on its own, before any recovery attempt";
			trace_(L"Display path restored after " + seconds(now - lost_since_) + L" " + how + L". Active: " + snapshot.active_description);
			reset();
		}
		return;
	}

	if (snapshot.connected_monitors == 0)
	{
		if (lost_)
		{
			trace_(L"Monitor disconnected while the display path was lost, after " + seconds(now - lost_since_) + L". Recovery stopped");
			reset();
		}
		return;
	}

	if (!lost_)
	{
		lost_ = true;
		lost_since_ = now;
		next_attempt_ = now + kAttemptDelays[0] * 1000ULL;
		trace_(L"No active display path while " + std::to_wstring(snapshot.connected_monitors) + L" monitor(s) connected (" + snapshot.monitor_description
			+ L"). Windows failed to enable the display. Recovery starts in " + std::to_wstring(kAttemptDelays[0]) + L" s");
		return;
	}

	if (paused_ || now < next_attempt_)
		return;
	if (attempts_ < kMaxAttempts)
		attemptRecovery(snapshot);
	else
	{
		paused_ = true;
		trace_(L"Recovery paused after " + std::to_wstring(attempts_) + L" attempts over " + seconds(now - lost_since_)
			+ L". The display path is still lost. Recovery resumes on the next display change or display power on event");
	}
}

void DisplayGuard::rearm(void)
{
	if (!lost_ || !paused_)
		return;
	paused_ = false;
	attempts_ = 0;
	last_attempt_ = 0;
	next_attempt_ = GetTickCount64() + kAttemptDelays[0] * 1000ULL;
	trace_(L"Display event received while the display path is lost. Recovery resumes");
}

UINT DisplayGuard::checkInterval(void) const
{
	return lost_ && !paused_ ? kIntervalWhileRecovering : kIntervalNormal;
}

void DisplayGuard::attemptRecovery(const Snapshot& snapshot)
{
	const Method& method = kMethods[attempts_ % std::size(kMethods)];
	attempts_++;
	last_attempt_ = GetTickCount64();
	last_attempt_result_ = SetDisplayConfig(0, NULL, 0, NULL, method.flags);
	next_attempt_ = last_attempt_ + (attempts_ < kMaxAttempts ? kAttemptDelays[attempts_] * 1000ULL : kSettleMs);
	trace_(L"Recovery attempt " + std::to_wstring(attempts_) + L"/" + std::to_wstring(kMaxAttempts) + L" after " + seconds(last_attempt_ - lost_since_)
		+ L": " + method.description + L" (" + snapshot.monitor_description + L"). SetDisplayConfig returned " + resultText(last_attempt_result_));
}

void DisplayGuard::reset(void)
{
	lost_ = false;
	paused_ = false;
	attempts_ = 0;
	lost_since_ = 0;
	next_attempt_ = 0;
	last_attempt_ = 0;
	last_attempt_result_ = ERROR_SUCCESS;
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
