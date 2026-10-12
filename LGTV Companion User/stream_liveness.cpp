#include "stream_liveness.h"
#include <tlhelp32.h>
#include <pdhmsg.h>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace
{
	// Process names of streaming hosts whose sessions are judged (Apollo ships the same executable name).
	const wchar_t* const	kHosts[] = { L"sunshine.exe" };
	// Summed utilisation of the host's video encode engines above which it counts as streaming. A live stream
	// encodes at least at the minimum frame rate, which keeps the engines well above this.
	constexpr double		kActiveThreshold = 0.5;

	DWORD findHost(void)
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
			return 0;
		DWORD pid = 0;
		PROCESSENTRY32W entry = { sizeof(entry) };
		for (BOOL more = Process32FirstW(snapshot, &entry); more && pid == 0; more = Process32NextW(snapshot, &entry))
			for (const wchar_t* host : kHosts)
				if (_wcsicmp(entry.szExeFile, host) == 0)
					pid = entry.th32ProcessID;
		CloseHandle(snapshot);
		return pid;
	}
}

StreamLiveness::~StreamLiveness()
{
	close();
}

StreamLiveness::State StreamLiveness::sample(void)
{
	if (process_ && WaitForSingleObject(process_, 0) != WAIT_TIMEOUT)
		close();	// the host exited (or restarted)
	if (!process_ && !attach())
		return State::Unknown;

	if (PdhCollectQueryData(query_) != ERROR_SUCCESS)
		return State::Unknown;
	if (!primed_)
	{
		primed_ = true;	// the first collection only establishes the baseline
		return State::Unknown;
	}

	DWORD size = 0;
	DWORD count = 0;
	if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, NULL) != PDH_MORE_DATA)
		return State::Unknown;	// no encode engine instances for this process
	std::vector<BYTE> buffer(size);
	auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
	if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS)
		return State::Unknown;

	double utilisation = 0;
	for (DWORD i = 0; i < count; i++)
		if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
			utilisation += items[i].FmtValue.doubleValue;
	return utilisation > kActiveThreshold ? State::Active : State::Idle;
}

bool StreamLiveness::attach(void)
{
	const DWORD pid = findHost();
	if (pid == 0)
		return false;
	process_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!process_)
		return false;
	// e.g. \GPU Engine(pid_2600_luid_0x00000000_0x01605f1a_phys_0_eng_6_engtype_videoencode)\Utilization Percentage
	const std::wstring path = L"\\GPU Engine(pid_" + std::to_wstring(pid) + L"_*engtype_VideoEncode)\\Utilization Percentage";
	if (PdhOpenQueryW(NULL, 0, &query_) != ERROR_SUCCESS || PdhAddEnglishCounterW(query_, path.c_str(), 0, &counter_) != ERROR_SUCCESS)
	{
		close();
		return false;
	}
	return true;
}

void StreamLiveness::close(void)
{
	if (query_)
		PdhCloseQuery(query_);
	if (process_)
		CloseHandle(process_);
	query_ = NULL;
	counter_ = NULL;
	process_ = NULL;
	primed_ = false;
}
