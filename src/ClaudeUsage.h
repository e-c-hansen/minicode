// ClaudeUsage.h — pure C++17. A forecast of Claude Code's plan usage (A13):
// how much of the 5-hour and weekly windows is used, when each would reach
// 100% at the current rate, and how much of it this machine's transcripts
// do not explain.
//
// Two sources, both read only:
//
// - Claude Code's transcripts (`~/.claude/projects/**/*.jsonl`). Every
//   assistant line carries the request's token counts in `message.usage`.
//   `parseUsageLine` reads them from a line without decoding the rest of it
//   (a reply's text and tool inputs are stepped over), `Tally` drops the
//   copies Claude Code writes of one response, and `Scanner` reads the
//   files incrementally. The format is Claude Code's own and changes, so a
//   line that does not read is skipped, never an error.
// - The plan percentages Claude Code hands its status line on stdin
//   (`rate_limits.five_hour` and `.seven_day`: `used_percentage` and
//   `resets_at`), which `mutible statusline` (ClaudeStatusLine.h) logs as
//   `Sample`s, one line each, in mutible's own folder.
//
// Tokens become cost units: US dollars at Anthropic's API list prices
// (`priceFor`), so a model's weight against another is its price ratio.
// That is the prior; the fit (`fitScale`) then finds one number per window,
// the plan percent one cost unit is worth, from the samples. A rise in the
// percent that the transcripts do not explain (another machine, claude.ai,
// the cloud) is reported as unseen usage and kept out of the fit: the scale
// is a weighted median of the readings' ratios, so a minority of readings
// with unseen usage in them does not move it.
//
// The rules for reading the transcripts, deduplicating responses, pricing
// cache writes by their lifetime, the 5-hour blocks and the burn rate are
// ported from ccusage (https://github.com/ccusage/ccusage, MIT License,
// Copyright (c) 2025 ryoppippi): its Claude adapter's dedupe key
// (rust/adapters/claude/src/lib.rs, usage_dedupe_hash), its cost rules
// (rust/crates/ccusage-core/src/cost.rs: a 5-minute cache write at the
// model's cache-write price, a 1-hour one at twice the input price) and
// its blocks (rust/crates/ccusage/src/blocks.rs: identify_session_blocks,
// calculate_burn_rate). Nothing of ccusage is linked or vendored.
#pragma once
#include "AgentTranscript.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ClaudeUsage {

// ------------------------------------------------------------- tokens
struct Tokens {
    uint64_t input = 0;
    uint64_t write5m = 0;     // cache writes kept 5 minutes
    uint64_t write1h = 0;     // cache writes kept an hour
    uint64_t cacheRead = 0;
    uint64_t output = 0;
    uint64_t total() const { return input + write5m + write1h + cacheRead + output; }
    void add(const Tokens &o);
};

// One response's usage, as read from a transcript line.
struct Entry {
    double time = 0;            // Unix seconds
    std::string sessionId;
    std::string model;          // message.model, as written
    std::string messageId;      // message.id
    std::string requestId;      // requestId
    bool sidechain = false;     // a subagent's line
    bool fast = false;          // usage.speed == "fast"
    bool detailed = false;      // the cache write's lifetimes were given
    Tokens tokens;
    double cost = 0;            // cost units (costOf), filled by Tally::add
};

// Reads one transcript line (no newline). True for an assistant line with
// a usage object whose input and output counts are numbers; false for any
// other line, a line that is not JSON, or one whose counts are all zero
// (Claude Code's own `<synthetic>` replies). The session is the line's
// sessionId; `fallbackSession` (the file's name) when it has none.
bool parseUsageLine(const char *data, size_t length, const std::string &fallbackSession,
                    Entry *out);

// ------------------------------------------------------------- prices
// US dollars per million tokens. Cache writes are priced from `input`:
// 1.25 times for 5 minutes, 2 times for an hour (Anthropic's rule).
struct Price {
    double input = 0, output = 0, cacheRead = 0;
    // Above `longContext` tokens of input (input, cache writes and reads
    // together) the whole request costs the `long*` prices; 0 for none.
    uint64_t longContext = 0;
    double longInput = 0, longOutput = 0, longCacheRead = 0;
    double fastMultiplier = 2;  // speed "fast"
};

// The price of a model id as Claude Code writes it ("claude-opus-5-5",
// "claude-sonnet-4-5-20250929", "us.anthropic.claude-opus-4-8-v1:0",
// "claude-opus-5-5[1m]"). `known` false when the id is not in the table:
// the newest price of its family (opus, sonnet, haiku, fable, mythos), or
// the Opus price when not even that reads.
Price priceFor(const std::string &model, bool *known = nullptr);
// The model without its date, platform prefix and suffixes
// ("claude-opus-4-5"), as the table and the per-model tally name it.
std::string normalizeModel(const std::string &model);
// Cost units (dollars at API prices) of one response.
double costOf(const Tokens &t, const Price &p, bool fast);

// ------------------------------------------------------------- the tally
struct SessionModel {
    Tokens tokens;
    double cost = 0;
    int responses = 0;
};

// Every response read, each once. A response Claude Code wrote several
// times (one line per content block, a subagent's copy of its parent's
// reply, a resumed session's replay) is the same message id and request
// id; without a request id the key is the message id and the session,
// and the time too unless the id is Anthropic's own (`msg_`), since a
// gateway may reuse one id for every reply (ccusage's rule). Of two
// copies the main thread's is kept over a subagent's, then the one with
// more tokens, then the one that says more (speed, cache lifetimes).
class Tally {
public:
    // Prices the entry and keeps it, or replaces a copy. True when it
    // added or replaced.
    bool add(Entry e);
    const std::vector<Entry> &entries() const { return entries_; }
    // Entries by time (a copy, sorted).
    std::vector<Entry> sorted() const;
    // Per session, then per model (normalizeModel's).
    std::map<std::string, std::map<std::string, SessionModel>> bySession() const;
    std::map<std::string, SessionModel> byModel() const;
    void clear();

private:
    std::vector<Entry> entries_;
    std::unordered_map<std::string, size_t> index_;
};

// Reads the transcripts under a projects folder (`<projects>/<folder>/
// *.jsonl` and `<projects>/<folder>/<session>/subagents/*.jsonl`) into a
// Tally, each file from where the last scan stopped, whole lines only.
// Files last changed before `since` are skipped. A file that was replaced
// or shrank is read again from its start (the tally drops the copies).
class Scanner {
public:
    struct Stats {
        int files = 0;           // files looked at
        uint64_t bytesRead = 0;
        int entries = 0;         // usage lines added or replaced
        int skipped = 0;         // lines too long to read
    };
    Stats scan(const AgentTranscript::Fs &fs, const std::string &projectsDir, double since,
               Tally &tally);

private:
    struct FileState {
        uint64_t offset = 0, inode = 0, device = 0;
    };
    std::map<std::string, FileState> files_;
    void readFile(const AgentTranscript::Fs &fs, const std::string &path, Tally &tally,
                  Stats &stats);
};

// ------------------------------------------------------------- blocks
// ccusage's 5-hour blocks, rebuilt from the entries alone: a block starts
// at the hour of its first entry and ends 5 hours later; the next entry
// after its end, or after a 5-hour silence, starts another.
struct Block {
    double start = 0, end = 0;   // end = start + 5 h
    double first = 0, last = 0;  // first and last entry
    double cost = 0;
    Tokens tokens;
    int entries = 0;
    bool active = false;         // now is before its end and within 5 h of its last entry
};
std::vector<Block> blocks(const std::vector<Entry> &sortedEntries, double now);

// ccusage's burn rate of a block: its cost over the time from its first to
// its last entry, per hour; 0 for a block of one instant.
double blockCostPerHour(const Block &b);

// Cost units between two times (after `from`, up to and including `to`).
double costBetween(const std::vector<Entry> &sortedEntries, double from, double to);

// Per session, the cost per hour over the last `window` seconds before
// now, for sessions with an entry in it, busiest first.
struct SessionRate {
    std::string sessionId;
    double costPerHour = 0;
    double last = 0;
};
std::vector<SessionRate> sessionRates(const std::vector<Entry> &sortedEntries, double now,
                                      double window);

// ------------------------------------------------------------- samples
// One status line reading. A percent is -1 when Claude Code did not give
// that window (an API key's session, before the first reply, or after it
// reset); a reset time is Unix seconds.
//
// Each Claude Code session runs its own status line with the percentages
// of its own last reply, so a session that has been idle for an hour
// reports an hour-old reading when its status line runs again (the user
// types into it). `time` is when the line was logged; the reading itself
// is as old as the session's last reply, which `readingTimes` finds.
struct Sample {
    double time = 0;
    double fivePct = -1, fiveReset = 0;
    double weekPct = -1, weekReset = 0;
    std::string session;   // the status line's session_id; empty when not given
    bool hasFive() const { return fivePct >= 0 && fiveReset > 0; }
    bool hasWeek() const { return weekPct >= 0 && weekReset > 0; }
};

// The status line's stdin JSON -> a sample taken at `now`. False when it is
// not JSON or has neither window.
bool sampleFromStatusJson(const std::string &json, double now, Sample *out);

// The log's line for a sample (no newline): "v1 <time> <5h %> <5h reset>
// <week %> <week reset> [<session>]", "-" for a missing window. Times are
// whole seconds, percents up to two decimals; the session (letters, digits,
// '-', '_' and '.', at most 64) only when known.
std::string formatSample(const Sample &s);
// A log line back; false for anything else (another version, a torn line).
bool parseSample(const std::string &line, Sample *out);
// Every sample in a log's text, oldest first, bad lines skipped.
std::vector<Sample> parseLog(const std::string &text);

// Is `next` an older reading than `last`: in some window it has an earlier
// reset, or the same reset and a lower percent (a plan's percent only
// rises until its window resets), and in none is it newer? That is a
// second session's stale reading, not news.
bool olderReading(const Sample &last, const Sample &next);

// Should `next` be logged after `last` (the log's last line)? Not when it
// is an older reading (`olderReading`); else when a window's percent or
// reset changed, or `minGap` seconds have gone by.
bool worthLogging(const Sample &last, const Sample &next, double minGap = 60);

// The samples at their readings' own times, oldest first: a sample with a
// session is as old as that session's last reply at or before it was
// logged (the newest of `sortedEntries` with its session id), and a
// session's sample that repeats a reading already logged is dropped. A
// sample whose session has no entries keeps its logged time. The fit and
// the forecast read samples through this, so an idle session's stale
// reading sorts in where it was taken instead of after the busy session's
// newer ones.
std::vector<Sample> readingTimes(const std::vector<Sample> &samples,
                                 const std::vector<Entry> &sortedEntries);

// ------------------------------------------------------------- the forecast
enum class Window { FiveHour, Week };
double windowLength(Window w);   // 5 h or 7 d, in seconds

// The plan percent one cost unit is worth in a window, from the samples
// (at their readings' times, `readingTimes`; a reading older than one
// before it, `olderReading`'s rule, is left out rather than starting the
// window over). Each window (one reset time) is cut into readings: from
// its start (reset minus the window's length) to its first sample, then on
// from sample to sample until the cost or the percent has moved enough to
// measure. Each
// reading gives percent over cost; the scale is their median weighted by
// cost. A reading's percent above scale times cost is unseen usage.
struct Fit {
    bool ok = false;
    double scale = 0;            // percent per cost unit
    int readings = 0;            // readings with cost in them
};
Fit fitScale(const std::vector<Sample> &samples, const std::vector<Entry> &sortedEntries,
             Window w);

struct WindowForecast {
    bool known = false;          // a sample of this window exists
    bool estimated = false;      // the window reset since the last sample: worked out
    double pctNow = 0;           // the last sample plus what was used since
    double resetsAt = 0;
    double sampledAt = 0;        // the last sample's time
    double pctPerHour = 0;       // at the current rate
    double pctPerHourLow = 0, pctPerHourHigh = 0;
    // When it reaches 100% at the current rate (and the range from the
    // rates' spread); 0 when it would not before it resets, or the rate is 0.
    double hitAt = 0, hitEarliest = 0, hitLatest = 0;
    double pctAtReset = 0;       // where the current rate leaves it at the reset
    double unseenPct = 0;        // percent points the transcripts do not explain
    Fit fit;
};

struct Forecast {
    WindowForecast five, week;
    double costPerHour = 0;      // the last hour's rate, all sessions
    double costInBlock = 0;      // the current 5-hour block's cost
    std::vector<SessionRate> active;   // sessions with entries in the last 15 minutes
    int entries = 0;
};

// How far back the transcripts are read (8 days): enough for a week's
// window and its start. The forecast takes the entries to cover that span.
constexpr double kHorizon = 8 * 24 * 3600;

Forecast forecast(const std::vector<Sample> &samples, const std::vector<Entry> &sortedEntries,
                  double now);

// The forecast as lines of prose for the panel (every port shows these):
// each window, the burn rate, the unseen usage, the fit.
std::vector<std::string> describe(const Forecast &f, double now);
// "45 m", "2 h 10 m", "3 d 4 h", "under a minute".
std::string formatDuration(double seconds);

}  // namespace ClaudeUsage
