// Copyright Foundry Media. FoundryFSDK Unreal subsystem facade.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FoundryFSDKSubsystem.generated.h"

// Owns the fsdk-core handles + a lock that serializes core access across worker
// threads. Defined in the .cpp so this public header doesn't leak the C ABI.
struct FFsdkCoreState;

/** Blueprint-facing match status, mirrors fsdk_match_status from the C ABI. */
UENUM(BlueprintType)
enum class EFoundryMatchStatus : uint8
{
	Pending    UMETA(DisplayName = "Pending"),
	Searching  UMETA(DisplayName = "Searching"),
	Found      UMETA(DisplayName = "Found"),
	Cancelled  UMETA(DisplayName = "Cancelled"),
	Failed     UMETA(DisplayName = "Failed"),
	Expired    UMETA(DisplayName = "Expired"),
	Unknown    UMETA(DisplayName = "Unknown")
};

/**
 * Blueprint-facing result, mirrors fsdk_result. Lets game code branch on the
 * outcome - e.g. re-acquire a token on Unauthorized, retry on Network/Timeout,
 * keep polling on NoMatch.
 */
UENUM(BlueprintType)
enum class EFoundryFsdkResult : uint8
{
	Ok               UMETA(DisplayName = "OK"),
	InvalidArg       UMETA(DisplayName = "Invalid Argument"),
	NotAuthenticated UMETA(DisplayName = "Not Authenticated"),
	Unauthorized     UMETA(DisplayName = "Unauthorized"),
	Network          UMETA(DisplayName = "Network Error"),
	Timeout          UMETA(DisplayName = "Timeout"),
	Protocol         UMETA(DisplayName = "Protocol Error"),
	NoMatch          UMETA(DisplayName = "No Match / Not Ready"),
	NotImplemented   UMETA(DisplayName = "Not Implemented"),
	Unavailable      UMETA(DisplayName = "No Servers Available"),
	Internal         UMETA(DisplayName = "Internal Error"),
	Unknown          UMETA(DisplayName = "Unknown"),
	// Appended after S1 shipped (FSDK_ERR_RATE_LIMITED) - never reorder the
	// values above; existing Blueprint switches key on them by index.
	RateLimited      UMETA(DisplayName = "Rate Limited")
};

/** Blueprint-facing connection details, mirrors fsdk_connection. */
USTRUCT(BlueprintType)
struct FFoundryConnection
{
	GENERATED_BODY()

	/** Opaque rendezvous host (the box today, a relay endpoint in future). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	FString Ip;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	int32 Port = 0;

	/** Short-lived FID-signed match token; forward to the server on connect. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	FString MatchToken;
};

/**
 * The platform's "am I already seated?" answer, mirrors fsdk_session_seat.
 * bActive = the player is seated in a LIVE match and can reconnect to it
 * (ReconnectMatch); inactive = search fresh.
 */
USTRUCT(BlueprintType)
struct FFoundrySessionSeat
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	bool bActive = false;

	/** The seat's ticket id - feed ReconnectMatch / AbandonMatch. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	FString TicketId;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	FString MatchId;

	/** The queue's submit key, e.g. "conquest/classic". */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	FString QueueKey;
};

// Completion delegates - broadcast on the GAME THREAD when an async op finishes.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFoundryFsdkResultEvent, EFoundryFsdkResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFsdkStatusEvent, EFoundryFsdkResult, Result, EFoundryMatchStatus, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFsdkConnectionEvent, EFoundryFsdkResult, Result, FFoundryConnection, Connection);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFsdkSessionSeatEvent, EFoundryFsdkResult, Result, FFoundrySessionSeat, Seat);

// Auth completion delegates - broadcast on the GAME THREAD.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryLoginEvent, EFoundryFsdkResult, Result, const FString&, DisplayName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFoundryLoggedOutEvent);

/** Blueprint-facing chat channel, mirrors fsdk_chat_channel from the C ABI.
 *  Every channel is a room subscription MULTIPLEXED over the one realtime
 *  socket - adding a channel never adds a connection. */
UENUM(BlueprintType)
enum class EFoundryChatChannel : uint8
{
	Global UMETA(DisplayName = "Global"),
	Party  UMETA(DisplayName = "Party"),
	/** Match-wide all-chat: every player seated in the match. */
	Match  UMETA(DisplayName = "Match"),
	/** Your team only - the server resolves WHICH team from your seated
	 *  ticket; the client never states a team index. */
	Team   UMETA(DisplayName = "Team")
};

/** One friend from the redacted in-game social read (fid GameScope). */
USTRUCT(BlueprintType)
struct FFoundryFriend
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString FoundryId;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString Username;

	/** Effective presence: online|idle|away|dnd|offline (invisible reads offline). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString Presence;

	/** Running game title, empty when none ("In game: Conquest"). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString PresenceGame;
};

/** One whisper conversation summary (newest activity first). */
USTRUCT(BlueprintType)
struct FFoundryDmConversation
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString FoundryId;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString Presence;

	/** Preview of the newest message (truncated server text). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString LastBody;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	int64 Unread = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	bool bLastFromMe = false;
};

/** One whisper message, caller-oriented (bFromMe flips the bubble side). */
USTRUCT(BlueprintType)
struct FFoundryDmMessage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	int64 Id = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	bool bFromMe = false;

	/** Retracted tombstone - body is empty; render an "unsent" quip. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	bool bUnsent = false;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString Body;

	/** ISO-8601 server stamp. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString CreatedAt;
};

// Chat delegates - broadcast on the GAME THREAD.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FFoundryChatMessageEvent,
	EFoundryChatChannel, Channel, const FString&, DisplayName, const FString&, FoundryId, const FString&, Body);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFoundryChatStateEvent, bool, bReady);

/** One party member. State is "JOINED" or "INVITED" (pending accept). */
USTRUCT(BlueprintType)
struct FFoundryPartyMember
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString FoundryId;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString Username;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString State;
};

/** The player's current party (PartyId empty when not in one). */
USTRUCT(BlueprintType)
struct FFoundryParty
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString PartyId;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString LeaderFoundryId;

	/** The leader's display name (the inviter, on invite rows). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	FString LeaderDisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	int32 MaxSize = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|Social")
	TArray<FFoundryPartyMember> Members;
};

// Social delegates - broadcast on the GAME THREAD.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFriendsEvent,
	EFoundryFsdkResult, Result, const TArray<FFoundryFriend>&, Friends);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryPartyEvent,
	EFoundryFsdkResult, Result, const FFoundryParty&, Party);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryPartyInvitesEvent,
	EFoundryFsdkResult, Result, const TArray<FFoundryParty>&, Invites);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundrySocialActionEvent,
	EFoundryFsdkResult, Result, const FString&, Action);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFriendCodeEvent,
	EFoundryFsdkResult, Result, const FString&, Code);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryConversationsEvent,
	EFoundryFsdkResult, Result, const TArray<FFoundryDmConversation>&, Conversations);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFoundryWhisperHistoryEvent,
	EFoundryFsdkResult, Result, const FString&, FriendId, const TArray<FFoundryDmMessage>&, Messages);

// ═══════════════════════════════════════════════════════════════════════════
// FRC text chat (the chat box: channels, input line, slash commands, history)
// ═══════════════════════════════════════════════════════════════════════════
// Built on TOP of the FRC chat rooms (EFoundryChatChannel::Party) and the
// whisper (DM) surface above - see fsdk-core's "CLIENT TEXT CHAT API" block in
// foundry/fsdk.h. A game gives it ONE input line (SubmitChatLine) and takes ONE
// line stream back (OnChatLine + GetChatHistory); the slash-command parsing,
// whisper name resolution, whisper poll, and FRC-room auto-rejoin all happen
// inside fsdk_textchat, not in this binding.

/** Which conversational surface a text-chat line/command targets, mirrors
 *  fsdk_textchat_channel. Distinct from EFoundryChatChannel (the four FRC room
 *  slots above) - Party here rides EFoundryChatChannel::Party under the hood
 *  in the default Platform binding; Whisper rides the DM REST surface; System
 *  never has a wire representation. */
UENUM(BlueprintType)
enum class EFoundryTextChatChannel : uint8
{
	Party   UMETA(DisplayName = "Party"),
	Whisper UMETA(DisplayName = "Whisper"),
	System  UMETA(DisplayName = "System")
};

/** What kind of line was recorded, mirrors fsdk_textchat_line_kind - drives how
 *  a panel colors/tabs a line. */
UENUM(BlueprintType)
enum class EFoundryChatLineKind : uint8
{
	Chat       UMETA(DisplayName = "Chat"),
	WhisperIn  UMETA(DisplayName = "Whisper In"),
	WhisperOut UMETA(DisplayName = "Whisper Out"),
	System     UMETA(DisplayName = "System"),
	Error      UMETA(DisplayName = "Error")
};

/**
 * Which transport preset ConfigureTextChat selects, mirrors fsdk_textchat_mode.
 *   Platform (default) - Party rides an FRC room, Whisper rides the DM REST
 *     surface; every message is authorized/logged server-side.
 *   Local - Party is HOST-bound (the game supplies its own transport/netcode
 *     via InjectChatLine for inbound; there is no outbound send hook in this
 *     slice - see the class doc on ConfigureTextChat), Whisper answers "not
 *     available" (a whisper is a Foundry-account feature - it cannot exist
 *     without the platform). ZERO HTTP/WS calls are ever made in this mode.
 * System is ALWAYS local (echoed into history only) regardless of Mode.
 */
UENUM(BlueprintType)
enum class EFoundryTextChatMode : uint8
{
	Platform UMETA(DisplayName = "Platform"),
	Local    UMETA(DisplayName = "Local")
};

/** One recorded chat-box line (content or notice), mirrors fsdk_textchat_line. */
USTRUCT(BlueprintType)
struct FFoundryChatLine
{
	GENERATED_BODY()

	/** Monotonic per text-chat handle (resets on the next ConfigureTextChat), starts at 1. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	int64 Id = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	EFoundryTextChatChannel Channel = EFoundryTextChatChannel::Party;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	EFoundryChatLineKind Kind = EFoundryChatLineKind::Chat;

	/** Author; empty for System/Error lines. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	FString FromFoundryId;

	/** Author display name; "You" for the local player when no name is known. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	FString FromName;

	/** Whisper lines only: the OTHER side of the conversation. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	FString PeerFoundryId;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	FString PeerName;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	FString Body;

	/** fsdk_textchat_tick's now_ms when this line was recorded; 0 before the
	 *  first tick has run. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|TextChat")
	int64 TimestampMs = 0;
};

/**
 * ConfigureTextChat's config, mirrors fsdk_textchat_config MINUS the
 * per-channel bind-override table (Party/Whisper/System all follow Mode's
 * preset in this slice - a future slice can widen this if a game needs a
 * mixed binding, e.g. Party over its own netcode with Whisper still on the
 * platform).
 */
USTRUCT(BlueprintType)
struct FFoundryTextChatConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|TextChat")
	EFoundryTextChatMode Mode = EFoundryTextChatMode::Platform;

	/** Ring size per channel. <= 0 defaults to 200 in the core. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|TextChat")
	int32 HistoryLines = 200;

	/** Whisper poll cadence while SetWhisperPolling(true) is active. <= 0
	 *  defaults to 5000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|TextChat")
	int32 WhisperPollMs = 5000;

	/** Friends-cache refresh cadence (whisper /w name resolution). <= 0
	 *  defaults to 120000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|TextChat")
	int32 FriendsRefreshMs = 120000;

	/**
	 * Forward this subsystem's own WARN+ fsdk-core diagnostics (any module, not
	 * just text chat) into the System channel as System/Error lines, IN
	 * ADDITION to the existing UE_LOG routing (LogFoundryFSDKCore) - this never
	 * steals the process-wide fsdk_set_log_sink (FoundryFSDKInstallLogSink owns
	 * that for the whole module's lifetime); it chains onto it via a small
	 * auxiliary hook (FoundryFSDKSetAuxLogSink). Off by default.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|TextChat")
	bool bRouteSdkLogToSystem = false;
};

// Text chat delegates - broadcast on the GAME THREAD.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFoundryChatLineEvent, const FFoundryChatLine&, Line);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFoundryTextChatChannelEvent, EFoundryTextChatChannel, Channel);

/**
 * Chat slash-command handler: Args are the command's arguments (the leading
 * "/name" already stripped, quote-aware split) -> an optional response string
 * (recorded as a System line when non-empty).
 *
 * This mirrors FFoundryConsoleHandlerBP's SHAPE (FoundryConsoleSubsystem.h:33 -
 * native FString return, TArray<FString> args) as its OWN type rather than
 * literally reusing it: FoundryConsoleSubsystem.h includes FMMSSubsystem.h,
 * which includes THIS header (for EFoundryFsdkResult/EFoundryMatchStatus/
 * FFoundryConnection) - including FoundryConsoleSubsystem.h back from here
 * would be a genuine new circular header (untested elsewhere in this module),
 * so this slice declares a same-shaped delegate instead. A game with one
 * handler function for both surfaces binds it to a variable of either type -
 * the UFUNCTION signature is identical either way. See the S2 report for this
 * deviation from the plan's literal "reuse FFoundryConsoleHandlerBP" wording.
 */
DECLARE_DYNAMIC_DELEGATE_RetVal_OneParam(FString, FFoundryChatCommandHandlerBP, const TArray<FString>&, Args);

/**
 * FoundryFSDK game-client facade.
 *
 * A GameInstance subsystem wrapping the fsdk-core CLIENT C ABI. The core ABI is
 * synchronous and performs blocking HTTPS; this subsystem runs each call on a
 * WORKER THREAD and broadcasts an On...Complete delegate back on the game thread,
 * so the game thread never blocks on the network. Core access is serialized (the
 * core is not thread-safe), and the core handles live behind a shared,
 * ref-counted state so an in-flight worker can't outlive a freed client.
 *
 * SECURITY: the player's FID token is PASSED IN to Authenticate by game code and
 * is never stored by this subsystem, never persisted, never logged. No secrets
 * live here. See ../../fsdk-core/SECURITY.md.
 */
UCLASS()
class FOUNDRYFSDK_API UFoundryFSDKSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem lifecycle.
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Create the underlying fsdk-core client bound to a platform API base URL
	 * (e.g. "https://api.foundryplatform.app"). Synchronous (no network) - call
	 * once before Authenticate. @return true on success.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	bool InitializeClient(const FString& BaseUrl);

	/**
	 * Authenticate with the PLAYER'S OWN FID session token, obtained by game code
	 * through the platform's normal sign-in. The token is not persisted. Async:
	 * broadcasts OnAuthenticateComplete.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void Authenticate(const FString& PlayerToken);

	/**
	 * Request a match in a named queue. AttributesJson is an opaque, queue-specific
	 * JSON object string (may be empty). Async: broadcasts OnRequestMatchComplete.
	 *
	 * bAutoRegion (default true): measure latency to every selectable region (list
	 * cached ~5 min) and submit with the map - the matchmaker then places in the
	 * lowest-latency region with capacity. To PIN a region (or subset), put
	 * "region" (and/or "latencies") in AttributesJson - caller keys always win -
	 * or pass false for the legacy regionless submit.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void RequestMatch(const FString& Queue, const FString& AttributesJson, bool bAutoRegion = true);

	/** Poll the active ticket's status. Async: broadcasts OnPollMatchComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void PollMatch();

	/**
	 * Fetch connection details once the match is Found. Async: broadcasts
	 * OnGetConnectionComplete with {Ip, Port, MatchToken} - hand Ip/Port to the
	 * engine netcode and forward MatchToken to the server.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void GetConnection();

	/** Cancel the active ticket (best-effort, fire-and-forget). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void CancelMatch();

	/**
	 * Ask the platform whether this player is already seated in a LIVE match
	 * (GET /v1/fmms/my-session) - the reconnect-aware Find Match read. Async:
	 * broadcasts OnMySessionComplete. An older fid without the route answers
	 * NoMatch with an inactive seat - treat as "no seat".
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void QueryMySession();

	/**
	 * Reconnect to a live seat from QueryMySession: rebuild the ticket handle
	 * (it becomes the active ticket) and resolve its connection - fid re-mints a
	 * FRESH match token bound to the SAME match + persisted team. Async:
	 * broadcasts OnGetConnectionComplete, same handler as the normal flow.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void ReconnectMatch(const FString& TicketId);

	/**
	 * Decline a live seat: cancel that ticket server-side (best-effort,
	 * fire-and-forget) so my-session stops reporting it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void AbandonMatch(const FString& TicketId);

	// ── FRC chat (the game's GLOBAL room over the platform realtime socket) ─────
	// Server-authoritative end to end: the player token only ever grants ROOM
	// operations (fid pins player sockets room-only), membership + rate limits +
	// logging are enforced server-side. Requires an authenticated session
	// (AutoLoginFromLauncher). One chat session per game instance.

	/**
	 * Join this game's GLOBAL chat room by game slug (e.g. "conquest"): resolves
	 * the room, opens the realtime socket, authenticates, subscribes. Async:
	 * OnChatStateChanged(true) fires when the subscription is live; a dropped
	 * socket fires OnChatStateChanged(false) and the subsystem auto-rejoins with
	 * backoff until LeaveChat().
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void JoinGlobalChat(const FString& GameSlug);

	/**
	 * Join the player's PARTY chat room on the SAME socket (multiplexed - no
	 * second connection). PartyId comes from RefreshParty / OnPartyUpdated.
	 * Async: OnPartyChatStateChanged(true) when the subscription is live. On a
	 * socket drop the party subscription rides the global rejoin automatically.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void JoinPartyChat(const FString& PartyId);

	/** Unsubscribe the party channel (left/disbanded). Socket + global stay up. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void LeavePartyChat();

	/**
	 * Join the current MATCH's all-chat room on the SAME socket (multiplexed -
	 * every player seated in the match). MatchId comes from the FMMS flow
	 * (UFMMSSubsystem::GetCurrentMatchId). Async: OnMatchChatStateChanged(true)
	 * when the subscription is live.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void JoinMatchChat(const FString& MatchId);

	/**
	 * Join YOUR TEAM's room for the match (multiplexed). The server resolves
	 * WHICH team from the caller's seated ticket - the client never states a
	 * team index, so the enemy team's room is unjoinable. Async:
	 * OnTeamChatStateChanged(true) when the subscription is live.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void JoinTeamChat(const FString& MatchId);

	/** Unsubscribe BOTH the match and team channels (match over / traveled
	 *  out). Socket + the other channels stay up. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void LeaveMatchChat();

	/**
	 * Send to the GLOBAL channel (500-char server cap). The echo arrives via
	 * OnChatMessage like everyone else's copy. Async: OnChatSendComplete fires
	 * with the result (Unavailable until the room subscription is live).
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void SendChat(const FString& Body);

	/** Send to a specific joined channel (the multi-tab chat box). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void SendChatToChannel(EFoundryChatChannel Channel, const FString& Body);

	/** Leave every room + close the socket (stops the auto-rejoin). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Chat")
	void LeaveChat();

	/** Whether the GLOBAL subscription is live (game-thread snapshot). */
	UFUNCTION(BlueprintPure, Category = "Foundry|Chat")
	bool IsChatReady() const { return bChatReady; }

	/** Whether a channel's subscription is live (game-thread snapshot). */
	UFUNCTION(BlueprintPure, Category = "Foundry|Chat")
	bool IsChatChannelReady(EFoundryChatChannel Channel) const
	{
		switch (Channel)
		{
		case EFoundryChatChannel::Party: return bPartyChatReady;
		case EFoundryChatChannel::Match: return bMatchChatReady;
		case EFoundryChatChannel::Team:  return bTeamChatReady;
		default:                         return bChatReady;
		}
	}

	/** Every room message (including the caller's own echo). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Chat")
	FFoundryChatMessageEvent OnChatMessage;

	/** GLOBAL subscription went live (true) / dropped (false; auto-rejoin runs). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Chat")
	FFoundryChatStateEvent OnChatStateChanged;

	/** PARTY subscription went live / dropped. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Chat")
	FFoundryChatStateEvent OnPartyChatStateChanged;

	/** MATCH (all-chat) subscription went live / dropped. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Chat")
	FFoundryChatStateEvent OnMatchChatStateChanged;

	/** TEAM subscription went live / dropped. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Chat")
	FFoundryChatStateEvent OnTeamChatStateChanged;

	/** One SendChat finished (Ok, Unavailable before ready, InvalidArg, ...). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Chat")
	FFoundryFsdkResultEvent OnChatSendComplete;

	// ── In-game social (full social session; fid GameScope 2026-07-11) ──────────
	// The game session is a FULL social citizen: list/pending/add/accept/remove/
	// block friends, share + redeem friend codes, run the party lifecycle, and
	// use the whole whisper (DM) surface - every call server-authorized as the
	// player. Still launcher/account territory (server-rejected for game
	// tokens): presence SETTING and friend-code ROTATION. Whisper + all reads
	// are POLL-based: refresh on your own cadence (tab-open + a timer with idle
	// controls); player sockets never receive dm push frames.

	/** Fetch the friends list (name + presence). Async: OnFriendsUpdated. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void RefreshFriends();

	/** Fetch INCOMING friend requests (people awaiting your accept). Async:
	 *  OnPendingUpdated. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void RefreshPendingRequests();

	/** Send a friend request by unique username. Async: OnSocialActionComplete
	 *  ("friend.request"; NoMatch = no such user, Protocol = throttled). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void SendFriendRequest(const FString& Username);

	/** Accept an incoming request (RequesterFoundryId from OnPendingUpdated).
	 *  Async: OnSocialActionComplete ("friend.accept"). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void AcceptFriendRequest(const FString& RequesterFoundryId);

	/** Remove a friend. Async: OnSocialActionComplete ("friend.remove"). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void RemoveFriend(const FString& FriendFoundryId);

	/** Block / unblock a player. Async: OnSocialActionComplete
	 *  ("friend.block" / "friend.unblock"). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void BlockPlayer(const FString& FoundryId);

	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void UnblockPlayer(const FString& FoundryId);

	/** The player's own share code (FDY-XXXXXX). Async: OnFriendCodeReady. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void GetMyFriendCode();

	/** Redeem a pasted code/link code for an INSTANT friendship. Async:
	 *  OnSocialActionComplete ("code.redeem"). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void RedeemFriendCode(const FString& Code);

	/** Snapshot the player's current party (empty PartyId when none). Async:
	 *  OnPartyUpdated - feed a non-empty PartyId to JoinPartyChat. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void RefreshParty();

	/** Parties the player is INVITED to (LeaderDisplayName names the inviter).
	 *  Async: OnPartyInvitesUpdated. Poll it with the social panel open. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void RefreshPartyInvites();

	/** Create a party. Async: OnSocialActionComplete ("party.create"), then an
	 *  automatic RefreshParty delivers the new party via OnPartyUpdated. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void CreateParty();

	/** Invite a FRIEND by username to a party you lead. Async:
	 *  OnSocialActionComplete ("party.invite"). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void InviteToParty(const FString& PartyId, const FString& Username);

	/** Accept / decline a party invite; leave a joined party. Async:
	 *  OnSocialActionComplete ("party.accept"/"party.decline"/"party.leave"),
	 *  each followed by an automatic RefreshParty. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void AcceptPartyInvite(const FString& PartyId);

	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void DeclinePartyInvite(const FString& PartyId);

	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void LeaveParty(const FString& PartyId);

	/** Fetch whisper conversation summaries. Async: OnConversationsUpdated. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void RefreshConversations();

	/** Fetch the newest page of one conversation (newest first). Async:
	 *  OnWhisperHistoryUpdated. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void RefreshWhisperHistory(const FString& FriendFoundryId);

	/** Whisper a friend (2000-char server cap; friends-only, server-enforced).
	 *  Async: OnWhisperSendComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void SendWhisper(const FString& FriendFoundryId, const FString& Body);

	/** Mark everything that friend sent as read (fire-and-forget). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Social")
	void MarkWhisperRead(const FString& FriendFoundryId);

	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundryFriendsEvent OnFriendsUpdated;

	/** Incoming friend requests (accept with AcceptFriendRequest). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundryFriendsEvent OnPendingUpdated;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundryPartyEvent OnPartyUpdated;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundryPartyInvitesEvent OnPartyInvitesUpdated;

	/** One social mutation finished; Action names it ("friend.request", ...). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundrySocialActionEvent OnSocialActionComplete;

	/** GetMyFriendCode finished. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundryFriendCodeEvent OnFriendCodeReady;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundryConversationsEvent OnConversationsUpdated;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundryWhisperHistoryEvent OnWhisperHistoryUpdated;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundryFsdkResultEvent OnWhisperSendComplete;

	// ── FRC text chat (the chat box: channels, slash commands, whisper) ─────────
	// A section built ON TOP of the FRC Party room above and the whisper (DM)
	// surface above (fsdk_textchat, see ../../fsdk-core/include/foundry/fsdk.h
	// "CLIENT TEXT CHAT API"). One input line (SubmitChatLine) in, one line
	// stream (OnChatLine) + a per-channel history ring (GetChatHistory) out.
	// Built-in slash commands: /w /whisper /msg /tell /dm <friend> <text>
	// (whisper), /r /reply <text> (reply to the last inbound whisper),
	// /p /party [text] (send, or with no text select Party as the active
	// channel), /help /? . /t /team /a /all /g /global are RESERVED (the other
	// FRC room slots above) and answer "not available in this game" until a
	// future slice wires them into this module.
	//
	// Party auto-binds to this subsystem's OWN party: whenever RefreshParty (or
	// the CreateParty/AcceptPartyInvite/DeclinePartyInvite/LeaveParty chains
	// that call it automatically) lands a party id on the game thread, this
	// subsystem calls fsdk_textchat_set_party itself - a game does nothing for
	// party chat beyond ConfigureTextChat once.

	/**
	 * (Re)configure the chat-box module. Starts the chat driver ticker if it
	 * isn't already running (today only a room join starts it). In Platform
	 * mode (the default) this creates the FRC chat handle if one doesn't
	 * already exist (shared with JoinGlobalChat/JoinPartyChat above) and MAY
	 * block briefly on a friends-list fetch, so the whole create runs on a
	 * WORKER THREAD; call it once at startup (or again to change Mode - the
	 * previous configuration's history/active channel/whisper target are
	 * discarded).
	 *
	 * In Platform mode, fsdk_textchat_create installs itself as the FRC chat
	 * handle's SOLE message callback (fsdk_chat_set_message_callback), so this
	 * re-installs FoundryFSDKChatMessageThunk as fsdk-core's ROOM PASS-THROUGH
	 * (fsdk_textchat_set_room_passthrough, fsdk-core 0a47316+a777915) right
	 * after - every raw room message (all four channels: Global/Party/Match/
	 * Team, including the caller's own echo) still reaches OnChatMessage
	 * exactly as it did before ConfigureTextChat existed; text chat separately
	 * records its own Party line into OnChatLine/GetChatHistory from the same
	 * message, via a distinct callback slot on the SAME handle.
	 *
	 * KNOWN LIMITATION (see the S2 report):
	 *  - Local mode's Party channel binds HOST by default (fsdk-core's preset),
	 *    but this slice never installs fsdk_textchat_set_host_send (it would
	 *    need to invoke a Blueprint delegate from whatever thread the core
	 *    calls it on, which can be a worker - unsafe for the Blueprint VM). So
	 *    Local-mode Party SENDS surface "No party chat transport." as an Error
	 *    line; InjectChatLine (the INBOUND half - the game's own netcode
	 *    delivered a message) works today. A native (non-Blueprint) host-send
	 *    hook is a follow-on slice.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	void ConfigureTextChat(const FFoundryTextChatConfig& Config);

	/**
	 * Submit one input-box line: a leading '/' is a slash command (a doubled
	 * "//" sends the literal, single-slashed text to the active channel);
	 * anything else goes to the active channel (SetActiveChatChannel). Runs on
	 * a WORKER THREAD (a Platform-bound send may block on the network). Every
	 * outcome - a sent message, an unknown command, a rejected send - is
	 * reported as a recorded line via OnChatLine, never through the result
	 * event; OnChatLineSubmitted only reports the SUBMIT call itself (Ok, or
	 * InvalidArg for an empty/whitespace-only line - nothing recorded).
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	void SubmitChatLine(const FString& Line);

	/** Which channel plain (non-slash) input targets. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	void SetActiveChatChannel(EFoundryTextChatChannel Channel);

	/** Game-thread snapshot - kept in sync by the chat driver tick (so a /p
	 *  with no text, which switches the active channel INSIDE the core, is
	 *  reflected here too). */
	UFUNCTION(BlueprintPure, Category = "Foundry|TextChat")
	EFoundryTextChatChannel GetActiveChatChannel() const { return ActiveChatChannelMirror; }

	/**
	 * Copy this channel's history (oldest -> newest) into OutLines, or every
	 * channel merged by id when bAllChannels is true (Channel is then ignored).
	 * In-memory only (no network) but still core state, so this briefly takes
	 * the core lock (TryLock - matching the chat driver tick, so a worker
	 * mid-send never stalls the game thread; a missed attempt just leaves
	 * OutLines untouched and the NEXT call picks up fresh data).
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	void GetChatHistory(EFoundryTextChatChannel Channel, bool bAllChannels,
		TArray<FFoundryChatLine>& OutLines);

	/** Record a SYSTEM-channel notice from the host game (e.g. a game-state
	 *  announcement). bIsError records as kind Error, otherwise System. No
	 *  network - runs inline under the core lock. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	void PostSystemMessage(const FString& Text, bool bIsError);

	/**
	 * Enable/disable the whisper poll (Platform binding only; a no-op
	 * otherwise). Disable this whenever the chat box/whisper tab is hidden or
	 * unfocused - every poller needs idle controls. Enabling schedules an
	 * immediate poll on the next chat driver tick.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	void SetWhisperPolling(bool bEnabled);

	/**
	 * Record an INBOUND line from a HOST-bound transport (the game's own
	 * netcode delivered a party message, or is bridging some other
	 * whisper-like surface). Whisper records as kind WhisperIn with the peer
	 * set from FromName/FromFoundryId. Rejected on the System channel (use
	 * PostSystemMessage instead). No network - runs inline under the core lock.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	void InjectChatLine(EFoundryTextChatChannel Channel, const FString& FromName,
		const FString& FromFoundryId, const FString& Body);

	/**
	 * Register a host command reachable as "/<name>" (<= 23 ASCII chars, no
	 * whitespace/'/', must not collide with a built-in or a reserved name -
	 * t/team/a/all/g/global). Re-registering an existing name replaces its
	 * handler. The handler's returned string (if non-empty) is recorded as a
	 * System line. @return false on an invalid/reserved name or a full command
	 * table.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	bool RegisterChatCommand(const FString& Name, const FString& Help,
		const FFoundryChatCommandHandlerBP& Handler);

	/** Command-name completions (built-ins + registered) starting with Prefix
	 *  (no leading slash; empty matches everything) - for a panel's
	 *  autocomplete. No network - runs inline under the core lock. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|TextChat")
	void GetChatCommandCompletions(const FString& Prefix, TArray<FString>& OutNames);

	/** Every recorded chat-box line (content or notice). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|TextChat")
	FFoundryChatLineEvent OnChatLine;

	/** The active channel changed (SetActiveChatChannel, or a bare "/p"). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|TextChat")
	FFoundryTextChatChannelEvent OnActiveChatChannelChanged;

	/** SubmitChatLine's own outcome (Ok, or InvalidArg for an empty line) - NOT
	 *  the send's outcome; watch OnChatLine for the recorded System/Error line
	 *  a rejected/failed send produces. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|TextChat")
	FFoundryFsdkResultEvent OnChatLineSubmitted;

	// ── Auto-login (DEFAULT: the Foundry launcher's session daemon) ─────────────
	// The game gets a short-lived matchmaking token from the launcher's session
	// daemon over the local FOUNDRY_IPC handoff - no in-game credentials, nothing
	// long-lived stored in the game. No launcher session -> fail fast (no form).

	/**
	 * Auto-authenticate. DEFAULT: the launcher handoff (FOUNDRY_IPC). With no handoff
	 * (the editor, a bare exe), a build that carries FID auth - every non-Shipping
	 * build - falls back to TryResumeSession(): the session a developer persisted
	 * once with the console's `foundry login`, so the editor signs in on every start
	 * with no typing. Async: broadcasts OnLoginComplete (Ok on success;
	 * NotAuthenticated when there is neither a launcher session nor a persisted one,
	 * which the caller surfaces as "sign in through the Foundry launcher").
	 *
	 * The subsystem CALLS THIS ITSELF on the first frame (no game code needed; a
	 * transient network failure is retried twice; `-NoFoundryAutoLogin` on the
	 * command line hands the timing to the game). Calling it again is safe: while an
	 * attempt is in flight it is a no-op, afterwards it signs in again (a launcher
	 * session gets a fresh token). Bind OnLoginComplete, or read IsLoggedIn() /
	 * GetDisplayName() if you come late.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void AutoLoginFromLauncher();

	/**
	 * Set the player token directly (the launcher handoff / BYO identity): authenticate
	 * WITHOUT a login or the /v1/me/user probe - the caller already vouched. Synchronous.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void SetPlayerToken(const FString& PlayerToken);

	// ── FID-embedded in-game auth (OPT-IN build: FOUNDRY_FSDK_FID_AUTH=1) ────────
	// Declared ALWAYS (UHT forbids a UFUNCTION inside a non-editor #if), but their
	// BODIES + the underlying fsdk_login/keyring machinery compile ONLY when
	// FOUNDRY_FSDK_FID_AUTH=1. In the default (launcher) build these are inert stubs
	// (NotImplemented) and NO credential-login code ships - auth comes from the
	// launcher handoff (AutoLoginFromLauncher). See FoundryFSDKSubsystem.cpp.

	/** Log in with Foundry credentials (email/username + password). Async: OnLoginComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void Login(const FString& EmailOrUsername, const FString& Password, bool bRememberMe);

	/** Refresh the session from the stored/in-memory refresh token (rotates it). Async: OnLoginComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void Refresh();

	/** Resume a session from the persisted refresh token (no password). Async: OnLoginComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void TryResumeSession();

	/** Revoke the session server-side and clear the persisted + in-memory tokens. Async: OnLoggedOut. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void Logout();

	/** Override the auth host (dev; default https://auth.foundryplatform.app). Synchronous. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void SetAuthBaseUrl(const FString& AuthBaseUrl);

	/** Cached login state (game-thread snapshot - never blocks on the network). */
	UFUNCTION(BlueprintPure, Category = "Foundry|Auth")
	bool IsLoggedIn() const { return bIsLoggedIn; }

	UFUNCTION(BlueprintPure, Category = "Foundry|Auth")
	FString GetDisplayName() const { return CachedDisplayName; }

	UFUNCTION(BlueprintPure, Category = "Foundry|Auth")
	FString GetFoundryId() const { return CachedFoundryId; }

	/** Login established/refreshed (DisplayName empty for BYO). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Auth")
	FFoundryLoginEvent OnLoginComplete;

	/** Session cleared. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Auth")
	FFoundryLoggedOutEvent OnLoggedOut;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkResultEvent OnAuthenticateComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkResultEvent OnRequestMatchComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkStatusEvent OnPollMatchComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkConnectionEvent OnGetConnectionComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkSessionSeatEvent OnMySessionComplete;

private:
	/** Ensure a client exists (lazily create one bound to the default api base). */
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> EnsureClient();

	/** Apply a session result on the game thread: cache identity + broadcast OnLoginComplete. */
	void ApplyLoginResult(EFoundryFsdkResult Result, const FString& DisplayName, const FString& FoundryId);

	/** AutoLoginFromLauncher's body; false when an attempt was already in flight. */
	bool StartAutoLogin();

	/** Queue the startup sign-in (or a retry) on the core ticker, DelaySeconds from now. */
	void ScheduleStartupAutoLogin(float DelaySeconds);

	bool bAutoLoginInFlight = false;         // one AutoLoginFromLauncher attempt at a time
	bool bStartupAutoLoginInFlight = false;  // the in-flight attempt is the subsystem's own
	int32 StartupAutoLoginRetriesLeft = 0;   // transient Network/Timeout retries (startup only)
	FTSTicker::FDelegateHandle StartupAutoLoginTickHandle;

	/** Game thread (the chat driver tick): drain inbound WS frames into the chat
	 *  handle, run the keepalive, detect ready flips, drive the auto-rejoin. */
	bool ChatDriverTick(float DeltaSeconds);

	/** Kick one (re)join attempt on a worker (guarded by bChatJoinInFlight). */
	void StartChatJoin();

	/** Kick one channel-SUBSCRIPTION join (party/match/team) on a worker.
	 *  Multiplexes onto the global chat's socket via the shared join helper -
	 *  the SAME launcher re-mint retry path the global join uses. */
	void StartChatSubscriptionJoin(EFoundryChatChannel Channel, const FString& Key);

	/** Broadcast the per-channel state delegate. */
	void BroadcastChatChannelState(EFoundryChatChannel Channel, bool bReady);

	/** Run one social mutation on a worker and broadcast OnSocialActionComplete.
	 *  Call returns an fsdk_result as int32 (the C ABI stays out of this header);
	 *  bRefreshPartyOnOk chains a RefreshParty after party-shape changes. */
	void RunSocialAction(const FString& Action, bool bRefreshPartyOnOk,
		TFunction<int32(FFsdkCoreState&)> Call);

	/** Text-chat party auto-bind: called whenever RefreshParty's completion
	 *  (including the automatic RefreshParty the party-mutation chains run)
	 *  lands a party id on the game thread. A no-op when the id hasn't changed
	 *  or no text chat has been configured; otherwise dispatches
	 *  fsdk_textchat_set_party on a worker (Platform binding may join/leave an
	 *  FRC room over the network). */
	void ApplyTextChatPartyId(const FString& NewPartyId);

	/** Kick one text-chat tick (chat keepalive + rejoin + whisper poll/friends
	 *  refresh) on a worker, guarded by bTextChatTickInFlight so at most one
	 *  runs at a time. Called from ChatDriverTick every 0.25s once
	 *  ConfigureTextChat has run - the core's fsdk_textchat_tick can block on
	 *  the whisper-poll HTTP, so it must never run on the game thread. */
	void KickTextChatTick(int64 NowMs);

	/** Execute (on the GAME THREAD) any chat-command invocations staged by the
	 *  native command thunk during the last SubmitChatLine (a slash command
	 *  dispatches synchronously inside fsdk_textchat_submit, on the worker that
	 *  called it - the Blueprint handler itself can only run here). Records
	 *  each handler's non-empty response as a System line
	 *  (fsdk_textchat_system - brief core-lock hold, no network). */
	void DrainPendingChatCommands();

	/** Registered chat slash-command handlers, keyed by lower-cased name.
	 *  fsdk_textchat_register_command keeps only ONE native fn + user_data per
	 *  command name, so ONE thunk fires for all of them and looks the actual
	 *  Blueprint delegate up here by name (mirrors UFoundryConsoleSubsystem's
	 *  FCommandEntry table). Game-thread-only: written by RegisterChatCommand,
	 *  read by DrainPendingChatCommands. */
	TMap<FString, FFoundryChatCommandHandlerBP> ChatCommandHandlers;

	/** Ref-counted fsdk-core handles + serialization lock (owned). */
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> Core;

	// Game-thread-only login snapshot (read by the BlueprintPure getters without a
	// lock, so they never block on an in-flight network call). Updated in the
	// game-thread completion of Login/Refresh/TryResume/SetPlayerToken/Logout.
	bool bIsLoggedIn = false;
	bool bLauncherSession = false; // token came from the daemon handoff (re-mintable)
	FString CachedDisplayName;
	FString CachedFoundryId;

	// ── Chat state ──
	// The fsdk_chat handle lives inside FFsdkCoreState (same lock as the client -
	// the chat borrows the client's token). These mirrors are game-thread-only.
	bool bChatReady = false;
	bool bPartyChatReady = false;   // PARTY channel mirror (same driver tick)
	bool bMatchChatReady = false;   // MATCH channel mirror (same driver tick)
	bool bTeamChatReady = false;    // TEAM channel mirror (same driver tick)
	bool bChatDesired = false;      // JoinGlobalChat sets, LeaveChat clears
	bool bChatJoinInFlight = false; // one join worker at a time
	FString ChatGameSlug;           // the slug the auto-rejoin re-joins
	double NextChatRejoinTime = 0;  // backoff gate (FPlatformTime::Seconds)
	int32 ChatRejoinStrikes = 0;    // exponential backoff ladder
	FTSTicker::FDelegateHandle ChatTickHandle;

	// ── Text-chat state ──
	// The fsdk_textchat handle lives inside FFsdkCoreState (same lock as the
	// client/chat). These mirrors are game-thread-only.
	bool bTextChatConfigured = false;   // ConfigureTextChat has run at least once
	bool bTextChatTickInFlight = false; // one KickTextChatTick worker at a time
	EFoundryTextChatChannel ActiveChatChannelMirror = EFoundryTextChatChannel::Party;
	FString TextChatPartyId;            // last party id applied (ApplyTextChatPartyId)
};
