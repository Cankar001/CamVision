#pragma once

#include <Client-Core.h>

#include <string>

struct UpdateConfig
{
	/// <summary>
	/// The path, where the received files from the update server should be stored to
	/// </summary>
	std::string UpdateTargetPath;

	/// <summary>
	/// The update server ip
	/// </summary>
	std::string ServerIP;

	/// <summary>
	/// The update server port
	/// </summary>
	uint16 Port;
};

enum class UpdaterStatusCode
{
	NONE = 0,
	BAD_CRC,
	BAD_SIG,
	BAD_WRITE
};

struct UpdaterStatus
{
	uint32 Bytes;
	uint32 Total;
	UpdaterStatusCode Code;
};

class Updater
{
public:

	Updater(const UpdateConfig &config);
	~Updater();

	bool IsUpdateAvail() const;
	void Run();

private:

	UpdateConfig m_Config;
	Core::Socket *m_Socket = nullptr;
	Core::FileSystem *m_FileSystem = nullptr;
	Core::addr_t m_Host;

	Core::Buffer m_UpdateData;
	Core::Buffer m_UpdatePieces;

	int64 m_LastUpdateMS;
	int64 m_LastPieceMS;
	uint64 m_ClientToken;
	uint64 m_ServerToken;
	uint32 m_ClientVersion;
	uint32 m_ServerVersion;
	UpdaterStatusCode m_Status;
	bool m_IsFinished;
	bool m_IsUpdating;
	uint32 m_UpdateIdx;
};