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
	Unknown          UMETA(DisplayName = "Unknown")
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

// Completion delegates - broadcast on the GAME THREAD when an async op finishes.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFoundryFsdkResultEvent, EFoundryFsdkResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFsdkStatusEvent, EFoundryFsdkResult, Result, EFoundryMatchStatus, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFsdkConnectionEvent, EFoundryFsdkResult, Result, FFoundryConnection, Connection);

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
	Party  UMETA(DisplayName = "Party")
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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundrySocialActionEvent,
	EFoundryFsdkResult, Result, const FString&, Action);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFriendCodeEvent,
	EFoundryFsdkResult, Result, const FString&, Code);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryConversationsEvent,
	EFoundryFsdkResult, Result, const TArray<FFoundryDmConversation>&, Conversations);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFoundryWhisperHistoryEvent,
	EFoundryFsdkResult, Result, const FString&, FriendId, const TArray<FFoundryDmMessage>&, Messages);

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
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void RequestMatch(const FString& Queue, const FString& AttributesJson);

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
		return Channel == EFoundryChatChannel::Party ? bPartyChatReady : bChatReady;
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

	// ── Auto-login (DEFAULT: the Foundry launcher's session daemon) ─────────────
	// The game gets a short-lived matchmaking token from the launcher's session
	// daemon over the local FOUNDRY_IPC handoff - no in-game credentials, nothing
	// long-lived stored in the game. No launcher session -> fail fast (no form).

	/**
	 * Auto-authenticate from the launcher handoff (FOUNDRY_IPC). Async: broadcasts
	 * OnLoginComplete (Ok on success; NotAuthenticated when there's no launcher
	 * session, which the caller surfaces as "sign in through the Foundry launcher").
	 * The default sign-in for launcher-distributed games (e.g. Conquest).
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

private:
	/** Ensure a client exists (lazily create one bound to the default api base). */
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> EnsureClient();

	/** Apply a session result on the game thread: cache identity + broadcast OnLoginComplete. */
	void ApplyLoginResult(EFoundryFsdkResult Result, const FString& DisplayName, const FString& FoundryId);

	/** Game thread (the chat driver tick): drain inbound WS frames into the chat
	 *  handle, run the keepalive, detect ready flips, drive the auto-rejoin. */
	bool ChatDriverTick(float DeltaSeconds);

	/** Kick one (re)join attempt on a worker (guarded by bChatJoinInFlight). */
	void StartChatJoin();

	/** Run one social mutation on a worker and broadcast OnSocialActionComplete.
	 *  Call returns an fsdk_result as int32 (the C ABI stays out of this header);
	 *  bRefreshPartyOnOk chains a RefreshParty after party-shape changes. */
	void RunSocialAction(const FString& Action, bool bRefreshPartyOnOk,
		TFunction<int32(FFsdkCoreState&)> Call);

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
	bool bChatDesired = false;      // JoinGlobalChat sets, LeaveChat clears
	bool bChatJoinInFlight = false; // one join worker at a time
	FString ChatGameSlug;           // the slug the auto-rejoin re-joins
	double NextChatRejoinTime = 0;  // backoff gate (FPlatformTime::Seconds)
	int32 ChatRejoinStrikes = 0;    // exponential backoff ladder
	FTSTicker::FDelegateHandle ChatTickHandle;
};
