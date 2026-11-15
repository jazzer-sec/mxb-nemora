#pragma once
#include <cstddef>

namespace mxb {
namespace host {

// Minimal append-only log, written next to the injected DLL as
// mxb_physics.log. An injected overlay has no console and a crash takes the
// process with it, so milestones go to disk as they happen rather than being
// buffered. Each line is flushed and closed immediately for that reason.
//
// Worth keeping: the first crash in this project (a bad call into an engine
// function) was only diagnosable from the Windows event log, which gives a
// faulting offset and nothing else.
void LogInit();
void Logf(const char* fmt, ...);

// Append one complete capture to a separate file that survives DLL reloads.
bool LogDiagnostic(const char* text, char* savedPath, std::size_t pathCapacity);

}  // namespace host
}  // namespace mxb
