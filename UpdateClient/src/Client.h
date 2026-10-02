#pragma once

#include <Cam-Core.h>

#include <string>

#include "Message.h"

struct ClientConfig
{
	/// <summary>
	/// The path, where the received files from the update server should be stored to
	/// </summary>
	std::string UpdateTargetPath;

	/// <summary>
	/// The path, where the updates are installed and from where the CamClient gets started.
	/// </summary>
	std::string UpdateBinaryPath;

	/// <summary>
	/// The folder with a CamClient, which was not installed by an update (for example the build output, or the first installation on a device).
	/// It is started, if no update was installed into UpdateBinaryPath yet.
	/// </summary>
	std::string FallbackPath;

	/// <summary>
	/// The file with the trusted public key of the update server (pinned key). If it exists, only updates signed with this key are accepted.
	/// If it does not exist, the key sent by the server is trusted once (the first successful update) and stored here, all later updates must match it.
	/// To trust a new server key, delete this file on the client.
	/// </summary>
	std::string PublicKeyPath;

	/// <summary>
	/// The update server ip
	/// </summary>
	std::string ServerIP;

	/// <summary>
	/// The update server port
	/// </summary>
	uint16 Port;

	/// <summary>
	/// The public key, which is used to verify the updates (the pinned key, or the one from the server before the first update).
	/// </summary>
	Core::Crypto::key_t PublicKey = {};
};

enum class ClientStatusCode
{
	/// <summary>
	/// This is the idle state, where the client just listens for updates, which may be sent by the update server
	/// </summary>
	NONE = 0,

	/// <summary>
	/// The client received a different server version and therefore needs a local update
	/// </summary>
	NEEDS_UPDATE,

	/// <summary>
	/// The client received the same version from the server and will start the CamClient.
	/// </summary>
	UP_TO_DATE,

	/// <summary>
	/// The update was downloaded, the signature is valid and update.zip was written to disk, but it is not installed yet.
	/// </summary>
	DOWNLOADED,
	
	/// <summary>
	/// The signature is missing or not generated.
	/// </summary>
	BAD_SIG,
	
	/// <summary>
	/// An error occurred, when writing the update to disk.
	/// </summary>
	BAD_WRITE
};

struct ClientStatus
{
	uint32 Bytes;
	uint32 Total;
	ClientStatusCode Code;
};

class Client
{
public:

	Client(const ClientConfig &config);
	~Client();

	void RequestServerVersion();
	void Run();
	void Reset();

	void UpdateProgress(int64 now_ms, Core::addr_t addr);

private:

	void MessageLoop();
	bool LoadLocalVersion();
	uint32 ReadInstalledVersion();
	void LoadPinnedKey();

	/// <summary>
	/// Extracts update.zip into the install path and remembers the new version.
	/// </summary>
	bool InstallUpdate();

	/// <summary>
	/// Starts the CamClient from the install path (the last installed update), or from the fallback path, if no update was installed yet.
	/// </summary>
	bool StartCamClient();

private:

	static constexpr uint32 MAX_REQUESTS = 32;

	ClientConfig m_Config;
	Core::Socket *m_Socket = nullptr;
	Core::Crypto *m_Crypto = nullptr;
	Core::addr_t m_Host;

	Core::Buffer m_UpdateData;
	Core::Buffer m_UpdatePieces;

	int64 m_LastUpdateMS = 0;
	int64 m_LastPieceMS = 0;
	uint64 m_ClientToken = 0;
	uint64 m_ServerToken = 0;
	uint32 m_ClientVersion = 0;
	uint32 m_LocalVersion = 0;
	ClientStatus m_Status = {};
	bool m_KeyPinned = false;
	bool m_IsFinished = true;
	bool m_IsUpdating = false;
	uint32 m_UpdateIdx = 0;
	uint32 m_CurrentRecvAttempt = 0;
	Signature m_UpdateSignature;
};

