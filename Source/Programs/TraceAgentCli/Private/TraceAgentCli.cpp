// Copyright Epic Games, Inc. All Rights Reserved.

#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Containers/StringConv.h"
#include "CoreGlobals.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "RequiredProgramMainCPPInclude.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "CborReader.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

#include <fcntl.h>
#include <io.h>
#include <limits>

DEFINE_LOG_CATEGORY_STATIC(LogTraceAgentCli, Log, All);

IMPLEMENT_APPLICATION(TraceAgentCli, "TraceAgentCli");

namespace UE::TraceAgentCli
{

using FJsonWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

struct FArguments
{
	FString Command;
	FString InputFile;
	FString Track;
	FString TimerIds;
	FString TimerName;
	FString BookmarkPattern;
	uint32 CounterId = uint32(-1);
	double StartTime = -std::numeric_limits<double>::infinity();
	double EndTime = std::numeric_limits<double>::infinity();
	int32 Limit = 1000;
	bool bCounterOps = false;
	bool bJsonOnly = false;
};

enum class ETrackKind : uint8
{
	CpuThread,
	GpuLegacy,
	GpuQueue,
	Verse,
	Frame,
};

struct FTrackSpec
{
	ETrackKind Kind;
	FString TrackId;
	FString Name;
	FString Group;
	uint32 SourceId = 0;
	uint32 TimelineIndex = uint32(-1);
	ETraceFrameType FrameType = TraceFrameType_Count;
};

class FScopedStandardStreamSilencer
{
public:
	FScopedStandardStreamSilencer()
	{
		const int StdOutFd = _fileno(stdout);
		const int StdErrFd = _fileno(stderr);

		SavedStdOutFd = _dup(StdOutFd);
		SavedStdErrFd = _dup(StdErrFd);
		errno_t OpenError = _sopen_s(&NullFd, "NUL", _O_WRONLY, _SH_DENYNO, 0);
		if (OpenError != 0)
		{
			NullFd = -1;
		}

		if (SavedStdOutFd >= 0 && SavedStdErrFd >= 0 && NullFd >= 0)
		{
			_dup2(NullFd, StdOutFd);
			_dup2(NullFd, StdErrFd);
			bActive = true;
		}
	}

	~FScopedStandardStreamSilencer()
	{
		if (bActive)
		{
			fflush(stdout);
			fflush(stderr);
			_dup2(SavedStdOutFd, _fileno(stdout));
			_dup2(SavedStdErrFd, _fileno(stderr));
		}

		if (SavedStdOutFd >= 0)
		{
			_close(SavedStdOutFd);
		}
		if (SavedStdErrFd >= 0)
		{
			_close(SavedStdErrFd);
		}
		if (NullFd >= 0)
		{
			_close(NullFd);
		}
	}

	void WriteLine(const FString& Text) const
	{
		if (!bActive || SavedStdOutFd < 0)
		{
			FTCHARToUTF8 Utf8(*Text);
			printf("%s\n", Utf8.Get());
			return;
		}

		const FString Line = Text + LINE_TERMINATOR;
		FTCHARToUTF8 Utf8(*Line);
		_write(SavedStdOutFd, Utf8.Get(), Utf8.Length());
	}

private:
	int SavedStdOutFd = -1;
	int SavedStdErrFd = -1;
	int NullFd = -1;
	bool bActive = false;
};

static TUniquePtr<FScopedStandardStreamSilencer> GStandardStreamSilencer;

static FString FrameTypeToToken(ETraceFrameType FrameType)
{
	switch (FrameType)
	{
	case TraceFrameType_Game:
		return TEXT("game");
	case TraceFrameType_Rendering:
		return TEXT("rendering");
	default:
		return TEXT("unknown");
	}
}

static FString CounterTypeToString(const TraceServices::ICounter& Counter)
{
	FString Type = Counter.IsFloatingPoint() ? TEXT("double") : TEXT("int64");
	if (Counter.IsResetEveryFrame())
	{
		Type += TEXT("|reset_every_frame");
	}
	return Type;
}

static FString CounterDisplayHintToString(TraceServices::ECounterDisplayHint Hint)
{
	switch (Hint)
	{
	case TraceServices::CounterDisplayHint_Memory:
		return TEXT("memory");
	case TraceServices::CounterDisplayHint_Bandwidth:
		return TEXT("bandwidth");
	case TraceServices::CounterDisplayHint_Percent:
		return TEXT("percent");
	default:
		return TEXT("none");
	}
}

static bool IsMetadataTimerIndex(uint32 TimerIndex)
{
	return int32(TimerIndex) < 0;
}

static void AppendMetadataToString(FString& InOut, TArrayView<const uint8> Metadata)
{
	FMemoryReaderView MemoryReader(Metadata);
	FCborReader CborReader(&MemoryReader, ECborEndianness::StandardCompliant);
	FCborContext Context;

	if (!CborReader.ReadNext(Context))
	{
		InOut += TEXT(" <empty>");
		return;
	}

	const bool bIsMap = Context.MajorType() == ECborCode::Map;
	for (uint32 Index = 0; true; ++Index)
	{
		if (bIsMap)
		{
			if (!CborReader.ReadNext(Context) || !Context.IsString())
			{
				break;
			}
		}

		if (Index > 0 || bIsMap)
		{
			if (!CborReader.ReadNext(Context))
			{
				break;
			}
		}

		if (Index == 0)
		{
			InOut += TEXT(" ");
		}
		else
		{
			InOut += TEXT(", ");
		}

		switch (Context.MajorType())
		{
		case ECborCode::Int:
			InOut += FString::Printf(TEXT("%lld"), Context.AsInt());
			continue;
		case ECborCode::Uint:
			InOut += FString::Printf(TEXT("%llu"), Context.AsUInt());
			continue;
		case ECborCode::TextString:
			InOut += Context.AsString();
			continue;
		case ECborCode::ByteString:
			InOut.AppendChars(Context.AsCString(), static_cast<int32>(Context.AsLength()));
			continue;
		default:
			break;
		}

		if (Context.RawCode() == (ECborCode::Prim | ECborCode::Value_4Bytes))
		{
			InOut += FString::Printf(TEXT("%f"), Context.AsFloat());
			continue;
		}

		if (Context.RawCode() == (ECborCode::Prim | ECborCode::Value_8Bytes))
		{
			InOut += FString::Printf(TEXT("%g"), Context.AsDouble());
			continue;
		}

		if (Context.RawCode() == (ECborCode::Prim | ECborCode::False))
		{
			InOut += TEXT("false");
			continue;
		}

		if (Context.RawCode() == (ECborCode::Prim | ECborCode::True))
		{
			InOut += TEXT("true");
			continue;
		}

		InOut += TEXT("???");
		if (Context.MajorType() == ECborCode::Array)
		{
			CborReader.SkipContainer(ECborCode::Array);
		}
	}
}

static FString GetEventMetadataText(const TraceServices::ITimingProfilerProvider& TimingProvider, const TraceServices::ITimingProfilerTimerReader& TimerReader, uint32 TimerIndex)
{
	if (!IsMetadataTimerIndex(TimerIndex))
	{
		return FString();
	}

	TArrayView<const uint8> Metadata = TimerReader.GetMetadata(TimerIndex);
	if (Metadata.IsEmpty())
	{
		return FString();
	}

	FString Text;
	AppendMetadataToString(Text, Metadata);
	return Text;
}

static void WriteErrorJson(const FString& Message)
{
	FString Output;
	TSharedRef<FJsonWriter> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("ok"), false);
	Writer->WriteValue(TEXT("error"), Message);
	Writer->WriteObjectEnd();
	Writer->Close();

	if (GStandardStreamSilencer)
	{
		GStandardStreamSilencer->WriteLine(Output);
	}
	else
	{
		FTCHARToUTF8 Utf8(*Output);
		printf("%s\n", Utf8.Get());
	}
}

static void PrintUsage()
{
	puts("TraceAgentCli");
	puts("Usage:");
	puts("  TraceAgentCli [global-options] <command> -i=<trace.utrace> [options]");
	puts("Global options:");
	puts("  --json-only            Suppress process stdout/stderr noise and emit only the final JSON result on stdout.");
	puts("Commands:");
	puts("  summary");
	puts("  list-tracks");
	puts("  list-timers");
	puts("  query-timing-events    [-track=<track_id>] [-start=<seconds>] [-end=<seconds>] [-limit=<n>] [-timer-ids=<id,id>] [-timer-name=<wildcard>]");
	puts("  list-counters");
	puts("  query-counter-values   -counter=<id> [-start=<seconds>] [-end=<seconds>] [-ops] [-limit=<n>]");
	puts("  query-bookmarks        [-start=<seconds>] [-end=<seconds>] [-pattern=<wildcard>] [-limit=<n>]");
	puts("Notes:");
	puts("  This is a standalone console program. It analyzes .utrace files directly and does not require launching Unreal Editor or Unreal Insights UI.");
}

static bool ParseUint32List(const FString& InValue, TSet<uint32>& OutValues)
{
	TArray<FString> Parts;
	InValue.ParseIntoArray(Parts, TEXT(","), true);
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty())
		{
			continue;
		}

		TCHAR* EndPtr = nullptr;
		const uint32 Value = FCString::Strtoui64(*Part, &EndPtr, 10);
		if (EndPtr == nullptr || *EndPtr != 0)
		{
			return false;
		}
		OutValues.Add(Value);
	}
	return true;
}

static bool ParseArgs(int32 ArgC, TCHAR* ArgV[], FArguments& OutArgs)
{
	if (ArgC < 2)
	{
		return false;
	}

	for (int32 Index = 1; Index < ArgC; ++Index)
	{
		const TCHAR* Arg = ArgV[Index];
		if (Arg[0] != TEXT('-') && OutArgs.Command.IsEmpty())
		{
			OutArgs.Command = Arg;
			continue;
		}

		if (FCString::Strnicmp(Arg, TEXT("-i="), 3) == 0)
		{
			OutArgs.InputFile = Arg + 3;
		}
		else if (FCString::Strnicmp(Arg, TEXT("-track="), 7) == 0)
		{
			OutArgs.Track = Arg + 7;
		}
		else if (FCString::Strnicmp(Arg, TEXT("-start="), 7) == 0)
		{
			OutArgs.StartTime = FCString::Atod(Arg + 7);
		}
		else if (FCString::Strnicmp(Arg, TEXT("-end="), 5) == 0)
		{
			OutArgs.EndTime = FCString::Atod(Arg + 5);
		}
		else if (FCString::Strnicmp(Arg, TEXT("-limit="), 7) == 0)
		{
			OutArgs.Limit = FCString::Atoi(Arg + 7);
		}
		else if (FCString::Strnicmp(Arg, TEXT("-timer-ids="), 11) == 0)
		{
			OutArgs.TimerIds = Arg + 11;
		}
		else if (FCString::Strnicmp(Arg, TEXT("-timer-name="), 12) == 0)
		{
			OutArgs.TimerName = Arg + 12;
		}
		else if (FCString::Strnicmp(Arg, TEXT("-counter="), 9) == 0)
		{
			OutArgs.CounterId = FCString::Strtoui64(Arg + 9, nullptr, 10);
		}
		else if (FCString::Strnicmp(Arg, TEXT("-pattern="), 9) == 0)
		{
			OutArgs.BookmarkPattern = Arg + 9;
		}
		else if (FCString::Stricmp(Arg, TEXT("-ops")) == 0)
		{
			OutArgs.bCounterOps = true;
		}
		else if (FCString::Stricmp(Arg, TEXT("--json-only")) == 0)
		{
			OutArgs.bJsonOnly = true;
		}
		else if (FCString::Stricmp(Arg, TEXT("-h")) == 0 || FCString::Stricmp(Arg, TEXT("--help")) == 0 || FCString::Stricmp(Arg, TEXT("-help")) == 0)
		{
			return false;
		}
		else
		{
			return false;
		}
	}

	return !OutArgs.Command.IsEmpty();
}

static bool ValidateArgs(const FArguments& Args, FString& OutError)
{
	if (Args.InputFile.IsEmpty())
	{
		OutError = TEXT("missing required argument: -i=<trace file>");
		return false;
	}

	if (!FPaths::FileExists(Args.InputFile))
	{
		OutError = FString::Printf(TEXT("trace file not found: %s"), *Args.InputFile);
		return false;
	}

	if (Args.Limit <= 0)
	{
		OutError = TEXT("limit must be greater than zero");
		return false;
	}

	if (Args.Command == TEXT("query-counter-values") && Args.CounterId == uint32(-1))
	{
		OutError = TEXT("query-counter-values requires -counter=<id>");
		return false;
	}

	return true;
}

static TSharedPtr<const TraceServices::IAnalysisSession> AnalyzeTrace(const FString& InputFile, FString& OutError)
{
	ITraceServicesModule& TraceServicesModule = FModuleManager::LoadModuleChecked<ITraceServicesModule>("TraceServices");
	TSharedPtr<TraceServices::IAnalysisService> AnalysisService = TraceServicesModule.GetAnalysisService();
	if (!AnalysisService.IsValid())
	{
		OutError = TEXT("unable to acquire TraceServices analysis service");
		return nullptr;
	}

	TSharedPtr<const TraceServices::IAnalysisSession> Session = AnalysisService->Analyze(*InputFile);
	if (!Session.IsValid())
	{
		OutError = FString::Printf(TEXT("failed to analyze trace file: %s"), *InputFile);
	}
	return Session;
}

static void EnumerateTimingTracks(const TraceServices::IAnalysisSession& Session, TArray<FTrackSpec>& OutTracks)
{
	const TraceServices::ITimingProfilerProvider* TimingProvider = TraceServices::ReadTimingProfilerProvider(Session);
	if (!TimingProvider)
	{
		return;
	}

	TraceServices::FAnalysisSessionReadScope SessionReadScope(Session);

	uint32 TimelineIndex = 0;
	if (TimingProvider->GetGpuTimelineIndex(TimelineIndex))
	{
		FTrackSpec& Track = OutTracks.AddDefaulted_GetRef();
		Track.Kind = ETrackKind::GpuLegacy;
		Track.TrackId = TEXT("gpu_legacy:1");
		Track.Name = TEXT("GPU1");
		Track.TimelineIndex = TimelineIndex;
	}

	if (TimingProvider->GetGpu2TimelineIndex(TimelineIndex))
	{
		FTrackSpec& Track = OutTracks.AddDefaulted_GetRef();
		Track.Kind = ETrackKind::GpuLegacy;
		Track.TrackId = TEXT("gpu_legacy:2");
		Track.Name = TEXT("GPU2");
		Track.TimelineIndex = TimelineIndex;
	}

	TimingProvider->EnumerateGpuQueues(
		[&OutTracks](const TraceServices::FGpuQueueInfo& QueueInfo)
		{
			FTrackSpec& Track = OutTracks.AddDefaulted_GetRef();
			Track.Kind = ETrackKind::GpuQueue;
			Track.TrackId = FString::Printf(TEXT("gpu_queue:%u"), QueueInfo.Id);
			Track.Name = QueueInfo.GetDisplayName();
			Track.SourceId = QueueInfo.Id;
			Track.TimelineIndex = QueueInfo.TimelineIndex;
		});

	if (TimingProvider->GetVerseTimelineIndex(TimelineIndex))
	{
		FTrackSpec& Track = OutTracks.AddDefaulted_GetRef();
		Track.Kind = ETrackKind::Verse;
		Track.TrackId = TEXT("verse:0");
		Track.Name = TEXT("VerseSampling");
		Track.TimelineIndex = TimelineIndex;
	}

	const TraceServices::IThreadProvider& ThreadProvider = TraceServices::ReadThreadProvider(Session);
	ThreadProvider.EnumerateThreads(
		[&OutTracks, TimingProvider](const TraceServices::FThreadInfo& ThreadInfo)
		{
			uint32 ThreadTimelineIndex = uint32(-1);
			if (!TimingProvider->GetCpuThreadTimelineIndex(ThreadInfo.Id, ThreadTimelineIndex))
			{
				return;
			}

			FTrackSpec& Track = OutTracks.AddDefaulted_GetRef();
			Track.Kind = ETrackKind::CpuThread;
			Track.TrackId = FString::Printf(TEXT("cpu_thread:%u"), ThreadInfo.Id);
			Track.Name = ThreadInfo.Name ? ThreadInfo.Name : TEXT("<unnamed>");
			Track.Group = ThreadInfo.GroupName ? ThreadInfo.GroupName : TEXT("");
			Track.SourceId = ThreadInfo.Id;
			Track.TimelineIndex = ThreadTimelineIndex;
		});

	for (uint32 FrameType = 0; FrameType < static_cast<uint32>(TraceFrameType_Count); ++FrameType)
	{
		FTrackSpec& Track = OutTracks.AddDefaulted_GetRef();
		Track.Kind = ETrackKind::Frame;
		Track.FrameType = static_cast<ETraceFrameType>(FrameType);
		Track.TrackId = FString::Printf(TEXT("frame:%s"), *FrameTypeToToken(Track.FrameType));
		Track.Name = FString::Printf(TEXT("Frame(%s)"), *FrameTypeToToken(Track.FrameType));
	}
}

static bool FindTrackById(const TArray<FTrackSpec>& Tracks, const FString& TrackId, FTrackSpec& OutTrack)
{
	for (const FTrackSpec& Track : Tracks)
	{
		if (Track.TrackId == TrackId)
		{
			OutTrack = Track;
			return true;
		}
	}
	return false;
}

static int32 CommandSummary(const TraceServices::IAnalysisSession& Session, const FString& InputFile)
{
	FString Output;
	TSharedRef<FJsonWriter> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);

	TArray<FTrackSpec> Tracks;
	TraceServices::FAnalysisSessionReadScope SessionReadScope(Session);
	EnumerateTimingTracks(Session, Tracks);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("ok"), true);
	Writer->WriteValue(TEXT("command"), TEXT("summary"));
	Writer->WriteValue(TEXT("trace_file"), InputFile);
	Writer->WriteValue(TEXT("trace_name"), Session.GetName());
	Writer->WriteValue(TEXT("duration_seconds"), Session.GetDurationSeconds());
	Writer->WriteValue(TEXT("trace_id"), static_cast<int64>(Session.GetTraceId()));
	Writer->WriteValue(TEXT("track_count"), Tracks.Num());

	if (const TraceServices::ITimingProfilerProvider* TimingProvider = TraceServices::ReadTimingProfilerProvider(Session))
	{
		Writer->WriteValue(TEXT("timing_timeline_count"), static_cast<int64>(TimingProvider->GetTimelineCount()));
	}
	const TraceServices::ICounterProvider& CounterProvider = TraceServices::ReadCounterProvider(Session);
	Writer->WriteValue(TEXT("counter_count"), static_cast<int64>(CounterProvider.GetCounterCount()));
	const TraceServices::IBookmarkProvider& BookmarkProvider = TraceServices::ReadBookmarkProvider(Session);
	Writer->WriteValue(TEXT("bookmark_count"), static_cast<int64>(BookmarkProvider.GetBookmarkCount()));
	Writer->WriteObjectEnd();
	Writer->Close();

	if (GStandardStreamSilencer)
	{
		GStandardStreamSilencer->WriteLine(Output);
	}
	else
	{
		FTCHARToUTF8 Utf8(*Output);
		printf("%s\n", Utf8.Get());
	}
	return 0;
}

static int32 CommandListTracks(const TraceServices::IAnalysisSession& Session)
{
	FString Output;
	TSharedRef<FJsonWriter> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);

	TArray<FTrackSpec> Tracks;
	EnumerateTimingTracks(Session, Tracks);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("ok"), true);
	Writer->WriteValue(TEXT("command"), TEXT("list-tracks"));
	Writer->WriteArrayStart(TEXT("tracks"));
	for (const FTrackSpec& Track : Tracks)
	{
		Writer->WriteObjectStart();
		switch (Track.Kind)
		{
		case ETrackKind::CpuThread:
			Writer->WriteValue(TEXT("kind"), TEXT("cpu_thread"));
			Writer->WriteValue(TEXT("thread_id"), static_cast<int64>(Track.SourceId));
			Writer->WriteValue(TEXT("timeline_index"), static_cast<int64>(Track.TimelineIndex));
			if (!Track.Group.IsEmpty())
			{
				Writer->WriteValue(TEXT("group"), Track.Group);
			}
			break;
		case ETrackKind::GpuLegacy:
			Writer->WriteValue(TEXT("kind"), TEXT("gpu_legacy"));
			Writer->WriteValue(TEXT("timeline_index"), static_cast<int64>(Track.TimelineIndex));
			break;
		case ETrackKind::GpuQueue:
			Writer->WriteValue(TEXT("kind"), TEXT("gpu_queue"));
			Writer->WriteValue(TEXT("queue_id"), static_cast<int64>(Track.SourceId));
			Writer->WriteValue(TEXT("timeline_index"), static_cast<int64>(Track.TimelineIndex));
			break;
		case ETrackKind::Verse:
			Writer->WriteValue(TEXT("kind"), TEXT("verse"));
			Writer->WriteValue(TEXT("timeline_index"), static_cast<int64>(Track.TimelineIndex));
			break;
		case ETrackKind::Frame:
			Writer->WriteValue(TEXT("kind"), TEXT("frame"));
			Writer->WriteValue(TEXT("frame_type"), FrameTypeToToken(Track.FrameType));
			break;
		}
		Writer->WriteValue(TEXT("track_id"), Track.TrackId);
		Writer->WriteValue(TEXT("name"), Track.Name);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	if (GStandardStreamSilencer)
	{
		GStandardStreamSilencer->WriteLine(Output);
	}
	else
	{
		FTCHARToUTF8 Utf8(*Output);
		printf("%s\n", Utf8.Get());
	}
	return 0;
}

static int32 CommandListTimers(const TraceServices::IAnalysisSession& Session)
{
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);

	const TraceServices::ITimingProfilerProvider* TimingProvider = TraceServices::ReadTimingProfilerProvider(Session);
	if (!TimingProvider)
	{
		WriteErrorJson(TEXT("timing profiler provider is unavailable"));
		return 1;
	}

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("ok"), true);
	Writer->WriteValue(TEXT("command"), TEXT("list-timers"));
	Writer->WriteArrayStart(TEXT("timers"));

	TraceServices::FAnalysisSessionReadScope SessionReadScope(Session);
	const TraceServices::ITimingProfilerTimerReader& TimerReader = TimingProvider->GetTimerReader();
	for (uint32 TimerIndex = 0; TimerIndex < TimerReader.GetTimerCount(); ++TimerIndex)
	{
		const TraceServices::FTimingProfilerTimer* Timer = TimerReader.GetTimer(TimerIndex);
		if (!Timer)
		{
			continue;
		}

		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("timer_index"), static_cast<int64>(TimerIndex));
		Writer->WriteValue(TEXT("timer_id"), static_cast<int64>(Timer->Id));
		switch (Timer->Type)
		{
		case TraceServices::ETimingProfilerTimerType::CpuScope:
			Writer->WriteValue(TEXT("type"), TEXT("cpu"));
			break;
		case TraceServices::ETimingProfilerTimerType::CpuSampling:
			Writer->WriteValue(TEXT("type"), TEXT("cpu_sampling"));
			break;
		case TraceServices::ETimingProfilerTimerType::GpuScope:
			Writer->WriteValue(TEXT("type"), TEXT("gpu"));
			break;
		case TraceServices::ETimingProfilerTimerType::VerseSampling:
			Writer->WriteValue(TEXT("type"), TEXT("verse"));
			break;
		default:
			Writer->WriteValue(TEXT("type"), TEXT("unknown"));
			break;
		}
		Writer->WriteValue(TEXT("name"), Timer->Name ? Timer->Name : TEXT(""));
		if (Timer->File)
		{
			Writer->WriteValue(TEXT("file"), Timer->File);
		}
		Writer->WriteValue(TEXT("line"), static_cast<int64>(Timer->Line));
		Writer->WriteObjectEnd();
	}

	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	if (GStandardStreamSilencer)
	{
		GStandardStreamSilencer->WriteLine(Output);
	}
	else
	{
		FTCHARToUTF8 Utf8(*Output);
		printf("%s\n", Utf8.Get());
	}
	return 0;
}

static bool EventMatchesFilter(const TraceServices::ITimingProfilerTimerReader& TimerReader, const TraceServices::FTimingProfilerEvent& Event, const TSet<uint32>& TimerIdFilter, const FString& TimerNamePattern)
{
	const TraceServices::FTimingProfilerTimer* Timer = TimerReader.GetTimer(Event.TimerIndex);
	if (!Timer)
	{
		return false;
	}

	uint32 EffectiveTimerId = Timer->Id;
	if (IsMetadataTimerIndex(Event.TimerIndex))
	{
		EffectiveTimerId = TimerReader.GetOriginalTimerIdFromMetadata(Event.TimerIndex);
	}

	if (TimerIdFilter.Num() > 0 && !TimerIdFilter.Contains(EffectiveTimerId))
	{
		return false;
	}

	if (!TimerNamePattern.IsEmpty() && !(Timer->Name && FString(Timer->Name).MatchesWildcard(TimerNamePattern)))
	{
		return false;
	}

	return true;
}

static void WriteTimingEventsFromTrack(FJsonWriter& Writer, const TraceServices::IAnalysisSession& Session, const TraceServices::ITimingProfilerProvider& TimingProvider, const FTrackSpec& Track, const FArguments& Args, const TSet<uint32>& TimerIdFilter, int32& InOutCount)
{
	const TraceServices::ITimingProfilerTimerReader& TimerReader = TimingProvider.GetTimerReader();

	auto WriteTimingEvent =
		[&Writer, &Track, &TimerReader, &TimingProvider, &Args, &TimerIdFilter, &InOutCount]
		(double StartTime, double EndTime, uint32 Depth, const TraceServices::FTimingProfilerEvent& Event)
		{
			if (InOutCount >= Args.Limit)
			{
				return TraceServices::EEventEnumerate::Stop;
			}

			if (!EventMatchesFilter(TimerReader, Event, TimerIdFilter, Args.TimerName))
			{
				return TraceServices::EEventEnumerate::Continue;
			}

			const TraceServices::FTimingProfilerTimer* Timer = TimerReader.GetTimer(Event.TimerIndex);
			if (!Timer)
			{
				return TraceServices::EEventEnumerate::Continue;
			}

			uint32 EffectiveTimerId = Timer->Id;
			if (IsMetadataTimerIndex(Event.TimerIndex))
			{
				EffectiveTimerId = TimerReader.GetOriginalTimerIdFromMetadata(Event.TimerIndex);
			}

			Writer.WriteObjectStart();
			Writer.WriteValue(TEXT("track_id"), Track.TrackId);
			Writer.WriteValue(TEXT("track_name"), Track.Name);
			Writer.WriteValue(TEXT("start_time"), StartTime);
			Writer.WriteValue(TEXT("end_time"), EndTime);
			Writer.WriteValue(TEXT("duration"), EndTime - StartTime);
			Writer.WriteValue(TEXT("depth"), static_cast<int64>(Depth));
			Writer.WriteValue(TEXT("timer_id"), static_cast<int64>(EffectiveTimerId));
			Writer.WriteValue(TEXT("timer_name"), Timer->Name ? Timer->Name : TEXT(""));
			if (Timer->File)
			{
				Writer.WriteValue(TEXT("timer_file"), Timer->File);
			}
			if (Track.Kind == ETrackKind::CpuThread)
			{
				Writer.WriteValue(TEXT("thread_id"), static_cast<int64>(Track.SourceId));
				if (!Track.Group.IsEmpty())
				{
					Writer.WriteValue(TEXT("thread_group"), Track.Group);
				}
			}
			else if (Track.Kind == ETrackKind::GpuQueue)
			{
				Writer.WriteValue(TEXT("queue_id"), static_cast<int64>(Track.SourceId));
			}

			const FString MetadataText = GetEventMetadataText(TimingProvider, TimerReader, Event.TimerIndex);
			if (!MetadataText.IsEmpty())
			{
				Writer.WriteValue(TEXT("metadata_text"), MetadataText);
			}
			Writer.WriteObjectEnd();

			++InOutCount;
			return InOutCount >= Args.Limit ? TraceServices::EEventEnumerate::Stop : TraceServices::EEventEnumerate::Continue;
		};

	if (Track.Kind == ETrackKind::Frame)
	{
		const TraceServices::IFrameProvider& FrameProvider = TraceServices::ReadFrameProvider(Session);
		FrameProvider.EnumerateFrames(Track.FrameType, Args.StartTime, Args.EndTime,
			[&Writer, &Track, &InOutCount, &Args](const TraceServices::FFrame& Frame)
			{
				if (InOutCount >= Args.Limit)
				{
					return;
				}

				Writer.WriteObjectStart();
				Writer.WriteValue(TEXT("track_id"), Track.TrackId);
				Writer.WriteValue(TEXT("track_name"), Track.Name);
				Writer.WriteValue(TEXT("frame_type"), FrameTypeToToken(Frame.FrameType));
				Writer.WriteValue(TEXT("frame_index"), static_cast<int64>(Frame.Index));
				Writer.WriteValue(TEXT("start_time"), Frame.StartTime);
				Writer.WriteValue(TEXT("end_time"), Frame.EndTime);
				Writer.WriteValue(TEXT("duration"), Frame.EndTime - Frame.StartTime);
				Writer.WriteObjectEnd();
				++InOutCount;
			});
		return;
	}

	if (Track.TimelineIndex == uint32(-1))
	{
		return;
	}

	TimingProvider.ReadTimeline(Track.TimelineIndex,
		[&WriteTimingEvent, &Args](const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
		{
			Timeline.EnumerateEvents(Args.StartTime, Args.EndTime, WriteTimingEvent);
		});
}

static int32 CommandQueryTimingEvents(const TraceServices::IAnalysisSession& Session, const FArguments& Args)
{
	const TraceServices::ITimingProfilerProvider* TimingProvider = TraceServices::ReadTimingProfilerProvider(Session);
	if (!TimingProvider)
	{
		WriteErrorJson(TEXT("timing profiler provider is unavailable"));
		return 1;
	}

	TArray<FTrackSpec> Tracks;
	EnumerateTimingTracks(Session, Tracks);

	TArray<FTrackSpec> QueryTracks;
	if (!Args.Track.IsEmpty())
	{
		FTrackSpec Track;
		if (!FindTrackById(Tracks, Args.Track, Track))
		{
			WriteErrorJson(FString::Printf(TEXT("unknown track id: %s"), *Args.Track));
			return 1;
		}
		QueryTracks.Add(Track);
	}
	else
	{
		QueryTracks = Tracks;
	}

	TSet<uint32> TimerIdFilter;
	if (!Args.TimerIds.IsEmpty() && !ParseUint32List(Args.TimerIds, TimerIdFilter))
	{
		WriteErrorJson(FString::Printf(TEXT("invalid timer id list: %s"), *Args.TimerIds));
		return 1;
	}

	FString Output;
	TSharedRef<FJsonWriter> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);

	TraceServices::FAnalysisSessionReadScope SessionReadScope(Session);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("ok"), true);
	Writer->WriteValue(TEXT("command"), TEXT("query-timing-events"));
	Writer->WriteArrayStart(TEXT("events"));
	int32 EventCount = 0;
	for (const FTrackSpec& Track : QueryTracks)
	{
		if (EventCount >= Args.Limit)
		{
			break;
		}
		WriteTimingEventsFromTrack(*Writer, Session, *TimingProvider, Track, Args, TimerIdFilter, EventCount);
	}
	Writer->WriteArrayEnd();
	Writer->WriteValue(TEXT("returned"), EventCount);
	Writer->WriteValue(TEXT("limit"), Args.Limit);
	Writer->WriteObjectEnd();
	Writer->Close();

	if (GStandardStreamSilencer)
	{
		GStandardStreamSilencer->WriteLine(Output);
	}
	else
	{
		FTCHARToUTF8 Utf8(*Output);
		printf("%s\n", Utf8.Get());
	}
	return 0;
}

static int32 CommandListCounters(const TraceServices::IAnalysisSession& Session)
{
	FString Output;
	TSharedRef<FJsonWriter> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);

	TraceServices::FAnalysisSessionReadScope SessionReadScope(Session);
	const TraceServices::ICounterProvider& CounterProvider = TraceServices::ReadCounterProvider(Session);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("ok"), true);
	Writer->WriteValue(TEXT("command"), TEXT("list-counters"));
	Writer->WriteArrayStart(TEXT("counters"));
	CounterProvider.EnumerateCounters(
		[&Writer](uint32 CounterId, const TraceServices::ICounter& Counter)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("counter_id"), static_cast<int64>(CounterId));
			Writer->WriteValue(TEXT("name"), Counter.GetName());
			Writer->WriteValue(TEXT("type"), CounterTypeToString(Counter));
			Writer->WriteValue(TEXT("display_hint"), CounterDisplayHintToString(Counter.GetDisplayHint()));
			if (Counter.GetGroup() && FCString::Strlen(Counter.GetGroup()) > 0)
			{
				Writer->WriteValue(TEXT("group"), Counter.GetGroup());
			}
			if (Counter.GetDescription() && FCString::Strlen(Counter.GetDescription()) > 0)
			{
				Writer->WriteValue(TEXT("description"), Counter.GetDescription());
			}
			Writer->WriteObjectEnd();
		});
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	if (GStandardStreamSilencer)
	{
		GStandardStreamSilencer->WriteLine(Output);
	}
	else
	{
		FTCHARToUTF8 Utf8(*Output);
		printf("%s\n", Utf8.Get());
	}
	return 0;
}

static int32 CommandQueryCounterValues(const TraceServices::IAnalysisSession& Session, const FArguments& Args)
{
	FString Output;
	TSharedRef<FJsonWriter> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);

	TraceServices::FAnalysisSessionReadScope SessionReadScope(Session);
	const TraceServices::ICounterProvider& CounterProvider = TraceServices::ReadCounterProvider(Session);

	bool bFoundCounter = false;
	int32 ValueCount = 0;

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("ok"), true);
	Writer->WriteValue(TEXT("command"), TEXT("query-counter-values"));
	Writer->WriteValue(TEXT("counter_id"), static_cast<int64>(Args.CounterId));
	Writer->WriteArrayStart(TEXT("values"));

	CounterProvider.ReadCounter(Args.CounterId,
		[&](const TraceServices::ICounter& Counter)
		{
			bFoundCounter = true;
			if (Args.bCounterOps)
			{
				if (Counter.IsFloatingPoint())
				{
					Counter.EnumerateFloatOps(Args.StartTime, Args.EndTime, false,
						[&](double Time, TraceServices::ECounterOpType Op, double Value)
						{
							if (ValueCount >= Args.Limit)
							{
								return;
							}
							Writer->WriteObjectStart();
							Writer->WriteValue(TEXT("time"), Time);
							Writer->WriteValue(TEXT("op"), Op == TraceServices::ECounterOpType::Set ? TEXT("set") : TEXT("add"));
							Writer->WriteValue(TEXT("value"), Value);
							Writer->WriteObjectEnd();
							++ValueCount;
						});
				}
				else
				{
					Counter.EnumerateOps(Args.StartTime, Args.EndTime, false,
						[&](double Time, TraceServices::ECounterOpType Op, int64 Value)
						{
							if (ValueCount >= Args.Limit)
							{
								return;
							}
							Writer->WriteObjectStart();
							Writer->WriteValue(TEXT("time"), Time);
							Writer->WriteValue(TEXT("op"), Op == TraceServices::ECounterOpType::Set ? TEXT("set") : TEXT("add"));
							Writer->WriteValue(TEXT("value"), Value);
							Writer->WriteObjectEnd();
							++ValueCount;
						});
				}
			}
			else
			{
				if (Counter.IsFloatingPoint())
				{
					Counter.EnumerateFloatValues(Args.StartTime, Args.EndTime, false,
						[&](double Time, double Value)
						{
							if (ValueCount >= Args.Limit)
							{
								return;
							}
							Writer->WriteObjectStart();
							Writer->WriteValue(TEXT("time"), Time);
							Writer->WriteValue(TEXT("value"), Value);
							Writer->WriteObjectEnd();
							++ValueCount;
						});
				}
				else
				{
					Counter.EnumerateValues(Args.StartTime, Args.EndTime, false,
						[&](double Time, int64 Value)
						{
							if (ValueCount >= Args.Limit)
							{
								return;
							}
							Writer->WriteObjectStart();
							Writer->WriteValue(TEXT("time"), Time);
							Writer->WriteValue(TEXT("value"), Value);
							Writer->WriteObjectEnd();
							++ValueCount;
						});
				}
			}
		});

	Writer->WriteArrayEnd();
	Writer->WriteValue(TEXT("returned"), ValueCount);
	Writer->WriteValue(TEXT("limit"), Args.Limit);
	Writer->WriteObjectEnd();
	Writer->Close();

	if (!bFoundCounter)
	{
		WriteErrorJson(FString::Printf(TEXT("unknown counter id: %u"), Args.CounterId));
		return 1;
	}

	if (GStandardStreamSilencer)
	{
		GStandardStreamSilencer->WriteLine(Output);
	}
	else
	{
		FTCHARToUTF8 Utf8(*Output);
		printf("%s\n", Utf8.Get());
	}
	return 0;
}

static int32 CommandQueryBookmarks(const TraceServices::IAnalysisSession& Session, const FArguments& Args)
{
	FString Output;
	TSharedRef<FJsonWriter> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);

	TraceServices::FAnalysisSessionReadScope SessionReadScope(Session);
	const TraceServices::IBookmarkProvider& BookmarkProvider = TraceServices::ReadBookmarkProvider(Session);

	int32 BookmarkCount = 0;
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("ok"), true);
	Writer->WriteValue(TEXT("command"), TEXT("query-bookmarks"));
	Writer->WriteArrayStart(TEXT("bookmarks"));
	BookmarkProvider.EnumerateBookmarks(Args.StartTime, Args.EndTime,
		[&](const TraceServices::FBookmark& Bookmark)
		{
			if (BookmarkCount >= Args.Limit)
			{
				return;
			}

			const FString Text = Bookmark.Text ? Bookmark.Text : TEXT("");
			if (!Args.BookmarkPattern.IsEmpty() && !Text.MatchesWildcard(Args.BookmarkPattern))
			{
				return;
			}

			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("time"), Bookmark.Time);
			Writer->WriteValue(TEXT("text"), Text);
			Writer->WriteValue(TEXT("callstack_id"), static_cast<int64>(Bookmark.CallstackId));
			Writer->WriteObjectEnd();
			++BookmarkCount;
		});
	Writer->WriteArrayEnd();
	Writer->WriteValue(TEXT("returned"), BookmarkCount);
	Writer->WriteValue(TEXT("limit"), Args.Limit);
	Writer->WriteObjectEnd();
	Writer->Close();

	if (GStandardStreamSilencer)
	{
		GStandardStreamSilencer->WriteLine(Output);
	}
	else
	{
		FTCHARToUTF8 Utf8(*Output);
		printf("%s\n", Utf8.Get());
	}
	return 0;
}

static int32 Run(const FArguments& Args)
{
	FString Error;
	TSharedPtr<const TraceServices::IAnalysisSession> Session = AnalyzeTrace(Args.InputFile, Error);
	if (!Session.IsValid())
	{
		WriteErrorJson(Error);
		return 1;
	}

	if (Args.Command == TEXT("summary"))
	{
		return CommandSummary(*Session, Args.InputFile);
	}
	if (Args.Command == TEXT("list-tracks"))
	{
		return CommandListTracks(*Session);
	}
	if (Args.Command == TEXT("list-timers"))
	{
		return CommandListTimers(*Session);
	}
	if (Args.Command == TEXT("query-timing-events"))
	{
		return CommandQueryTimingEvents(*Session, Args);
	}
	if (Args.Command == TEXT("list-counters"))
	{
		return CommandListCounters(*Session);
	}
	if (Args.Command == TEXT("query-counter-values"))
	{
		return CommandQueryCounterValues(*Session, Args);
	}
	if (Args.Command == TEXT("query-bookmarks"))
	{
		return CommandQueryBookmarks(*Session, Args);
	}

	WriteErrorJson(FString::Printf(TEXT("unknown command: %s"), *Args.Command));
	return 1;
}

} // namespace UE::TraceAgentCli

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	FTaskTagScope Scope(ETaskTag::EGameThread);
	ON_SCOPE_EXIT
	{
		LLM(FLowLevelMemTracker::Get().UpdateStatsPerFrame());
		RequestEngineExit(TEXT("Exiting"));
		FEngineLoop::AppPreExit();
		FModuleManager::Get().UnloadModulesAtShutdown();
		FEngineLoop::AppExit();
	};

	bool bJsonOnly = false;
	for (int32 Index = 1; Index < ArgC; ++Index)
	{
		if (FCString::Stricmp(ArgV[Index], TEXT("--json-only")) == 0)
		{
			bJsonOnly = true;
			break;
		}
	}

	if (bJsonOnly)
	{
		UE::TraceAgentCli::GStandardStreamSilencer = MakeUnique<UE::TraceAgentCli::FScopedStandardStreamSilencer>();
	}

	FCommandLine::Append(TEXT(" -unattended -NoLogTimes -NODEFAULTLOG -SILENT -LogCmds=\"global off\""));

	if (int32 Ret = GEngineLoop.PreInit(ArgC, ArgV))
	{
		return Ret;
	}

	GIsSilent = true;
	if (GLog && GLogConsole)
	{
		GLog->RemoveOutputDevice(GLogConsole);
	}

	UE::TraceAgentCli::FArguments Args;
	if (!UE::TraceAgentCli::ParseArgs(ArgC, ArgV, Args))
	{
		UE::TraceAgentCli::PrintUsage();
		return 1;
	}

	FString Error;
	if (!UE::TraceAgentCli::ValidateArgs(Args, Error))
	{
		UE::TraceAgentCli::WriteErrorJson(Error);
		return 1;
	}

	return UE::TraceAgentCli::Run(Args);
}
