#pragma once
#include <Windows.h>
#include <pdh.h>

// Tells whether a streaming host (Sunshine, Apollo) is actually streaming, by its use of the GPU video encoder.
//
// The host's own notion of a session is not reliable: a client that vanishes during a display failure can leave
// the host reporting a busy session for hours, without any client receiving video. A live stream encodes video
// continuously, so encoder utilisation of the host process is the ground truth. Uses the per-process "GPU Engine"
// performance counters, which work for any GPU vendor.
//
// Meant to be sampled only while a stream is believed to be running, at a low rate. The host process is looked up
// once and then held by handle, so a sample costs one performance counter collection.
class StreamLiveness
{
public:
	enum class State { Active, Idle, Unknown };	// Unknown: no streaming host process, or no encoder counters

	StreamLiveness() = default;
	~StreamLiveness();
	StreamLiveness(const StreamLiveness&) = delete;
	StreamLiveness& operator=(const StreamLiveness&) = delete;

	State		sample(void);
	// Release the process handle and the counter query (call when the stream ends).
	void		close(void);

private:
	bool		attach(void);

	HANDLE			process_ = NULL;	// the host, held to notice when it exits
	PDH_HQUERY		query_ = NULL;
	PDH_HCOUNTER	counter_ = NULL;
	bool			primed_ = false;	// rate counters need two collections
};
