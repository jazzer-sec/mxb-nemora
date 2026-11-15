#pragma once

namespace mxb {

// Locates a field in the bike physics object by capturing the whole object under
// known, deliberately opposite conditions and reporting which fields moved the
// way a field of that kind has to.
//
// This exists because guessing offsets is how this project produced its NaN
// crash and put the china hat at a fixed point outside the map. It is also a
// better instrument than static analysis for this particular job: it reports a
// field's live runtime range and sign behaviour, which is what identifies a
// control axis, rather than just the code that happens to write it.
//
// Typical use, for a steering axis: hold full left, capture A; let go, capture
// C; hold full right, capture B; compare with "Control axis 3pt". A centred
// -1..+1 control is then the only thing that can match.

enum ProbeMode {
    kProbeAxisF = 0,   // |v| <= 1.5, swapped sign between A and B
    kProbeAxis3F,      // the same, and C sits at centre -- much stronger
    kProbeSignFlipF,   // any float that swapped sign: rates, angles
    kProbeSignFlipD,
    kProbeChangedF,    // moved substantially
    kProbeChangedD,
    kProbeUnchangedF,  // did not move at all, for finding constants
    kProbeModeCount,
};

inline const char* const kProbeModes[] = {
    "Control axis (f)", "Control axis 3pt", "Sign flip (f)", "Sign flip (d)",
    "Changed (f)",      "Changed (d)",      "Unchanged (f)",
};

struct ProbeStatus {
    bool        have[3] = {false, false, false};
    const void* bike = nullptr;   // the bike the captures were taken from
    bool        mismatch = false; // captures came from different bikes
    int         matches = -1;     // -1 until a comparison has been run
    int         reported = 0;     // how many of them reached the log
};

// slot: 0 = A, 1 = B, 2 = C. Captures the first bike LoadedBikes() reports.
bool ProbeCapture(int slot);

// Writes every match to the log as an offset in both decimal and hex, ready to
// drop into Offsets.h. Returns the match count.
int ProbeCompare(int mode);

void ProbeClear();
ProbeStatus ProbeState();

}  // namespace mxb
