// Bounded GLog ring-buffer collector — see header for the cold-start /
// overflow / truncation caveats and the single-sink relationship to the
// per-call logs[] window.
#include "Tools/UnrealOpenMcpLogCollector.h"

#include "CoreGlobals.h"                     // GLog
#include "Misc/OutputDeviceRedirector.h"     // FOutputDeviceRedirector::Add/RemoveOutputDevice

FUnrealOpenMcpLogCollector& FUnrealOpenMcpLogCollector::Get()
{
	// Function-local static: constructed on first use, destroyed at process
	// exit. The editor module drives Start/Stop for GLog registration.
	static FUnrealOpenMcpLogCollector Instance;
	return Instance;
}

void FUnrealOpenMcpLogCollector::Start()
{
	FScopeLock ScopeLock(&Lock);
	if (bRegistered)
	{
		return;
	}
	if (Buffer.Num() != DefaultMaxEntries)
	{
		Buffer.Empty(DefaultMaxEntries);
		Buffer.SetNum(DefaultMaxEntries);
		Head = 0;
		Count = 0;
	}
	if (GLog != nullptr)
	{
		GLog->AddOutputDevice(this);
		bRegistered = true;
	}
}

void FUnrealOpenMcpLogCollector::Stop()
{
	// Unregister OUTSIDE the lock is not required, but GLog->RemoveOutputDevice
	// is self-synchronizing; take the lock only to flip the flag so a racing
	// Serialize sees a consistent bRegistered.
	{
		FScopeLock ScopeLock(&Lock);
		if (!bRegistered)
		{
			return;
		}
		bRegistered = false;
	}
	if (GLog != nullptr)
	{
		GLog->RemoveOutputDevice(this);
	}
}

void FUnrealOpenMcpLogCollector::Serialize(
	const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	// SetColor is a control marker (raw value, not a severity) that carries no
	// real message — skip it before masking.
	if (Verbosity == ELogVerbosity::SetColor)
	{
		return;
	}
	// Mask off the flag bits (BreakOnLog etc.) so only the severity level is
	// stored/compared.
	const ELogVerbosity::Type Level =
		static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
	if (Level == ELogVerbosity::NoLogging)
	{
		return;
	}

	FString Text(Message);
	if (Text.Len() > MaxMessageLen)
	{
		Text.LeftInline(MaxMessageLen, false);
		Text += TEXT("…[truncated]");
	}

	FScopeLock ScopeLock(&Lock);
	if (Buffer.Num() == 0)
	{
		// Not started (or already torn down) — nothing to write into.
		return;
	}

	FEntry Entry;
	Entry.Sequence = NextSequence++;
	Entry.Verbosity = Level;
	Entry.Category = Category;
	Entry.Message = MoveTemp(Text);
	Entry.Timestamp = FDateTime::UtcNow();

	if (Count < Buffer.Num())
	{
		const int32 Index = (Head + Count) % Buffer.Num();
		Buffer[Index] = MoveTemp(Entry);
		++Count;
	}
	else
	{
		// Full — overwrite the oldest and advance the head.
		Buffer[Head] = MoveTemp(Entry);
		Head = (Head + 1) % Buffer.Num();
	}
}

TArray<FUnrealOpenMcpLogCollector::FEntry> FUnrealOpenMcpLogCollector::Snapshot(
	const FFilter& Filter, int32& OutMatched) const
{
	OutMatched = 0;
	TArray<FEntry> Matches;

	FScopeLock ScopeLock(&Lock);
	if (Buffer.Num() == 0 || Count == 0)
	{
		return Matches;
	}

	const int32 Cap = Buffer.Num();
	// The min-severity threshold. ELogVerbosity::All (0) means "keep all"; the
	// numeric mask makes a lower value MORE severe, so an entry passes when its
	// level is numerically <= the threshold.
	const bool bFilterVerbosity =
		Filter.MinVerbosity != ELogVerbosity::All
		&& Filter.MinVerbosity <= ELogVerbosity::VeryVerbose;

	for (int32 i = 0; i < Count; ++i)
	{
		const FEntry& Entry = Buffer[(Head + i) % Cap];

		if (bFilterVerbosity && Entry.Verbosity > Filter.MinVerbosity)
		{
			continue;
		}
		if (!Filter.Category.IsNone() && Entry.Category != Filter.Category)
		{
			continue;
		}
		if (!Filter.Contains.IsEmpty()
			&& !Entry.Message.Contains(Filter.Contains, ESearchCase::IgnoreCase))
		{
			continue;
		}

		++OutMatched;
		Matches.Add(Entry);
	}

	// Keep only the most recent `Limit` matches (the tail of the ordered list).
	const int32 Limit = Filter.Limit > 0 ? Filter.Limit : Matches.Num();
	if (Matches.Num() > Limit)
	{
		Matches.RemoveAt(0, Matches.Num() - Limit, false);
	}
	return Matches;
}

int32 FUnrealOpenMcpLogCollector::Clear()
{
	FScopeLock ScopeLock(&Lock);
	const int32 Removed = Count;
	Head = 0;
	Count = 0;
	// NextSequence is intentionally NOT reset — sequence numbers stay monotonic
	// across a clear so an agent can tell pre- and post-clear entries apart.
	return Removed;
}

int32 FUnrealOpenMcpLogCollector::Num() const
{
	FScopeLock ScopeLock(&Lock);
	return Count;
}

ELogVerbosity::Type FUnrealOpenMcpLogCollector::ParseVerbosity(const FString& Token)
{
	const FString T = Token.TrimStartAndEnd();
	if (T.IsEmpty() || T.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		return ELogVerbosity::All;
	}
	if (T.Equals(TEXT("fatal"), ESearchCase::IgnoreCase)) return ELogVerbosity::Fatal;
	if (T.Equals(TEXT("error"), ESearchCase::IgnoreCase)) return ELogVerbosity::Error;
	if (T.Equals(TEXT("warning"), ESearchCase::IgnoreCase)) return ELogVerbosity::Warning;
	if (T.Equals(TEXT("display"), ESearchCase::IgnoreCase)) return ELogVerbosity::Display;
	if (T.Equals(TEXT("log"), ESearchCase::IgnoreCase)) return ELogVerbosity::Log;
	if (T.Equals(TEXT("verbose"), ESearchCase::IgnoreCase)) return ELogVerbosity::Verbose;
	if (T.Equals(TEXT("veryverbose"), ESearchCase::IgnoreCase)) return ELogVerbosity::VeryVerbose;
	// Not recognized — an out-of-range sentinel so the caller can reject it.
	return ELogVerbosity::NumVerbosity;
}

FString FUnrealOpenMcpLogCollector::VerbosityToString(ELogVerbosity::Type Verbosity)
{
	switch (static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask))
	{
	case ELogVerbosity::Fatal:       return TEXT("Fatal");
	case ELogVerbosity::Error:       return TEXT("Error");
	case ELogVerbosity::Warning:     return TEXT("Warning");
	case ELogVerbosity::Display:     return TEXT("Display");
	case ELogVerbosity::Log:         return TEXT("Log");
	case ELogVerbosity::Verbose:     return TEXT("Verbose");
	case ELogVerbosity::VeryVerbose: return TEXT("VeryVerbose");
	default:                         return TEXT("Log");
	}
}
