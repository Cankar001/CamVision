#include "Client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <assert.h>

#ifndef CAM_PLATFORM_WINDOWS
#include <sys/stat.h>
#endif

#include "Utils/Utils.h"
#include "Utils/ZipArchive.h"
#include "Core/Log.h"

static uint32 MAX_RECV_ATTEMPTS = 250;

// The version of the installed update. The version in the source code (CamVersion.h) does not change, when an update is installed,
// so the installed version is remembered in this file in the install path.
static const char *INSTALLED_VERSION_FILE = "installed_version.txt";

// The Windows implementation of WriteFile refuses to overwrite existing files, so an existing file is removed first.
static bool ReplaceFile(const std::string &path, const void *data, uint32 size)
{
	Core::FileSystem *fs = Core::FileSystem::Get();
	if (fs->FileExists(path) && !fs->RemoveFile(path))
	{
		return false;
	}

	return fs->WriteFile(path, (void *)data, size);
}

// File names from the archive must stay inside the install path.
static bool IsSafeFileName(const std::string &name)
{
	return !name.empty()
		&& name.find("..") == std::string::npos
		&& name.find(':') == std::string::npos
		&& name[0] != '/'
		&& name[0] != '\\';
}

Client::Client(const ClientConfig &config)
	: m_Config(config)
{
	m_Socket = Core::Socket::Create();
	m_Crypto = Core::Crypto::Create();

	std::string cwd = "";
	Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);

	LoadLocalVersion();
	LoadPinnedKey();

	CAM_LOG_INFO("===================== CONFIG ===================================");
	CAM_LOG_INFO("IP                    : {}", config.ServerIP);
	CAM_LOG_INFO("Port                  : {}", config.Port);
	CAM_LOG_INFO("Update binary path    : {}", config.UpdateBinaryPath);
	CAM_LOG_INFO("Update source path    : {}", config.UpdateTargetPath);
	CAM_LOG_INFO("Public key path       : {}", config.PublicKeyPath);
	CAM_LOG_INFO("Public key            : {}", m_KeyPinned ? "pinned" : "not pinned yet, trusting the first key from the server");
	CAM_LOG_INFO("Current Client version: {}", m_LocalVersion);
	CAM_LOG_INFO("Current CWD           : {}", cwd);
	CAM_LOG_INFO("================================================================");

	if (!m_Socket->Open(true, m_Config.ServerIP, m_Config.Port))
	{
		CAM_LOG_ERROR("Socket could not be opened!");
	}

	if (!m_Socket->SetNonBlocking(true))
	{
		CAM_LOG_ERROR("Socket could not be set to non-blocking!");
	}

	// Pieces arrive in bursts, a larger buffer keeps them from getting lost, while they wait to be processed.
	m_Socket->SetBufferSizes(1024 * 1024, 4 * 1024 * 1024);

	m_Host = m_Socket->Lookup(m_Config.ServerIP, m_Config.Port);
	Reset();
}

Client::~Client()
{
	delete m_Crypto;
	m_Crypto = nullptr;

	delete m_Socket;
	m_Socket = nullptr;
}

void Client::RequestServerVersion()
{
	// Then request the latest client version from the server
	CAM_LOG_DEBUG("Sending server version request...");
	ClientWantsVersionMessage message = {};
	message.LocalVersion = m_LocalVersion;
	message.ClientVersion = 0;
	message.Header.Type = MessageType::CLIENT_REQUEST_VERSION;
	message.Header.Version = m_LocalVersion;
	int32 bytes_sent = m_Socket->Send(&message, sizeof(message), m_Host);
	if (bytes_sent != (int32)sizeof(message))
	{
		CAM_LOG_ERROR("Could not send the version request to the update server!");
	}

	m_Status.Code = ClientStatusCode::NONE;
}

bool Client::QueryServerVersion(uint32 *out_version, uint32 timeout_ms)
{
	using Clock = std::chrono::steady_clock;
	auto elapsed_ms = [](Clock::time_point t) { return (int64)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); };

	ClientWantsVersionMessage request = {};
	request.LocalVersion = m_LocalVersion;
	request.ClientVersion = 0;
	request.Header.Type = MessageType::CLIENT_REQUEST_VERSION;
	request.Header.Version = m_LocalVersion;

	static Byte BUF[65536];
	Clock::time_point start = Clock::now();
	Clock::time_point last_request = start - std::chrono::milliseconds(1000);
	while (elapsed_ms(start) < (int64)timeout_ms)
	{
		// The request (or the answer) might get lost, so it is repeated until the server answers.
		if (elapsed_ms(last_request) >= 500)
		{
			m_Socket->Send(&request, sizeof(request), m_Host);
			last_request = Clock::now();
		}

		Core::addr_t addr = {};
		int32 len = m_Socket->Recv(BUF, sizeof(BUF), &addr);
		if (len <= 0)
		{
			Core::SleepMS(10);
			continue;
		}

		if (addr.Value != m_Host.Value || len != sizeof(ServerVersionInfoMessage))
		{
			continue;
		}

		header_t *header = (header_t *)BUF;
		if (header->Type != MessageType::SERVER_RECEIVE_VERSION)
		{
			continue;
		}

		*out_version = ((ServerVersionInfoMessage *)BUF)->Version;
		return true;
	}

	return false;
}

void Client::Run()
{
	// When running for the first time, request the server version
	RequestServerVersion();

	bool attempted_start = false;

	for (;;)
	{
		if (m_CurrentRecvAttempt >= MAX_RECV_ATTEMPTS)
		{
			CAM_LOG_WARN("The update server does not respond (anymore).");
			m_CurrentRecvAttempt = 0;
			break;
		}

		if (m_Status.Code == ClientStatusCode::DOWNLOADED)
		{
			// The running CamClient has to be gone, its files are locked (Windows) and it would run next to the one, which is started after the update.
			if (!StopCamClient())
			{
				CAM_LOG_ERROR("The running CamClient could not be stopped. The update is not installed.");
				m_Status.Code = ClientStatusCode::BAD_WRITE;
				continue;
			}

			if (InstallUpdate())
			{
				CAM_LOG_INFO("Update to version {} installed successfully.", m_LocalVersion);
			}
			else
			{
				m_Status.Code = ClientStatusCode::BAD_WRITE;
				continue;
			}

			attempted_start = true;
			StartCamClient();
			break;
		}
		else if (m_Status.Code == ClientStatusCode::UP_TO_DATE)
		{
			attempted_start = true;
			StartCamClient();
			break;
		}
		else if (m_Status.Code == ClientStatusCode::BAD_SIG)
		{
			CAM_LOG_ERROR("The signature of the update is not valid. The update is not installed.");
			break;
		}
		else if (m_Status.Code == ClientStatusCode::BAD_WRITE)
		{
			CAM_LOG_ERROR("The update could not be written to disk. The update is not installed.");
			break;
		}

		MessageLoop();

		// Process updates.
		int64 now_ms = Core::QueryMS();
		UpdateProgress(now_ms, m_Host);

		// Limit the update rate.
		Core::SleepMS(10);
	}

	if (!attempted_start)
	{
		// No update was possible (server not reachable, bad signature, ...). The camera must keep working with the version, which is installed.
		CAM_LOG_WARN("Starting the installed CamClient without an update...");
		StartCamClient();
	}
}

static std::string CamClientFile(const std::string &folder)
{
#if CAM_PLATFORM_WINDOWS
	return folder + "/CamClient.exe";
#else
	return folder + "/CamClient";
#endif
}

std::vector<uint32> Client::FindRunningCamClients()
{
	std::vector<uint32> result;
	for (const std::string &folder : { m_Config.UpdateBinaryPath, m_Config.FallbackPath })
	{
		if (folder.empty())
		{
			continue;
		}

		for (uint32 pid : Core::Process::FindByExecutable(CamClientFile(folder)))
		{
			if (std::find(result.begin(), result.end(), pid) == result.end())
			{
				result.push_back(pid);
			}
		}
	}

	return result;
}

bool Client::StopCamClient()
{
	std::vector<uint32> running = FindRunningCamClients();
	if (running.empty())
	{
		return true;
	}

	bool all_gone = true;
	for (uint32 pid : running)
	{
		CAM_LOG_INFO("Stopping the running CamClient (process {})...", pid);
		if (Core::Process::Stop(pid, m_Config.StopTimeoutSeconds))
		{
			CAM_LOG_INFO("The CamClient (process {}) was stopped.", pid);
		}
		else
		{
			CAM_LOG_ERROR("The CamClient (process {}) could not be stopped!", pid);
			all_gone = false;
		}
	}

	// The files of a program are released a moment after it ended.
	Core::SleepMS(500);
	return all_gone;
}

bool Client::StartCamClient()
{
	// A CamClient, which is running already (the update client is started, while the camera is working and no update is needed), must not get a twin: two
	// of them would send the same camera twice and fight for it.
	std::vector<uint32> running = FindRunningCamClients();
	if (!running.empty())
	{
		CAM_LOG_INFO("A CamClient is running already (process {}), not starting another one.", running.front());
		return true;
	}

	// Construct the path to the target executable
#if CAM_PLATFORM_WINDOWS
	const char *executable = "/CamClient.exe";
#else
	const char *executable = "/CamClient";
#endif

	// The last installed update wins. If there was no update yet, the CamClient, which was there from the beginning, is used.
	std::string camClientFile = m_Config.UpdateBinaryPath + executable;
	if (!Core::FileSystem::Get()->FileExists(camClientFile) && !m_Config.FallbackPath.empty())
	{
		std::string fallbackFile = m_Config.FallbackPath + executable;
		if (Core::FileSystem::Get()->FileExists(fallbackFile))
		{
			CAM_LOG_INFO("No update installed yet, using the CamClient from {}", m_Config.FallbackPath);
			camClientFile = fallbackFile;
		}
	}

	if (!Core::FileSystem::Get()->FileExists(camClientFile))
	{
		CAM_LOG_ERROR("There is no CamClient installed at {0} (or at {1})!", m_Config.UpdateBinaryPath + executable, m_Config.FallbackPath + executable);
		return false;
	}

#ifndef CAM_PLATFORM_WINDOWS
	// Files written to disk are not executable by default.
	chmod(camClientFile.c_str(), 0755);
#endif

	CAM_LOG_DEBUG("Starting CamClient...");
	if (!Core::FileSystem::Get()->StartProgram(camClientFile))
	{
		CAM_LOG_ERROR("Failed to start the Cam Client application!");
		return false;
	}

	CAM_LOG_INFO("CamClient started successfully.");
	return true;
}

bool Client::InstallUpdate()
{
	Core::FileSystem *fs = Core::FileSystem::Get();
	std::string zipFile = m_Config.UpdateBinaryPath + "/update.zip";

	if (!fs->FileExists(zipFile))
	{
		CAM_LOG_ERROR("The update file {} does not exist!", zipFile);
		return false;
	}

	CAM_LOG_DEBUG("Extracting zip archive...");
	Core::ZipArchive archive;
	std::vector<Core::ZipFile> files = archive.Load(zipFile);
	if (files.empty())
	{
		CAM_LOG_ERROR("The update archive is empty or could not be read!");
		return false;
	}

	CAM_LOG_INFO("zip archive extracted successfully.");

	// Run through the archive and store the files on the disk.
	CAM_LOG_DEBUG("Writing all files from archive to disk...");
	bool success = true;
	for (const auto &file : files)
	{
		if (!IsSafeFileName(file.Name))
		{
			CAM_LOG_ERROR("The archive contains the invalid file name {}, skipping it!", file.Name);
			success = false;
			continue;
		}

		std::string current_file = m_Config.UpdateBinaryPath + "/" + file.Name;
		CAM_LOG_DEBUG("    Writing file {} to disk...", current_file);

		if (!ReplaceFile(current_file, file.Buffer, (uint32)file.BufferSize))
		{
			// For example, because the program is still running.
			CAM_LOG_ERROR("Failed to store file {} on disk!", current_file);
			success = false;
		}
	}

	// Clean up the RAM memory
	for (auto &file : files)
	{
		delete[] (Byte *)file.Buffer;
		file.Buffer = nullptr;
	}

	// Remove zip file
	CAM_LOG_DEBUG("Trying to remove the update file...");
	if (!fs->RemoveFile(zipFile))
	{
		CAM_LOG_ERROR("Failed to remove file {}", zipFile);
	}
	else
	{
		CAM_LOG_INFO("Update file successfully removed.");
	}

	if (!success)
	{
		return false;
	}

	// Remember the new version, the next start of the update client has to know, that the update is installed.
	std::string version = std::to_string(m_ClientVersion);
	if (!ReplaceFile(m_Config.UpdateBinaryPath + "/" + INSTALLED_VERSION_FILE, version.data(), (uint32)version.size()))
	{
		CAM_LOG_ERROR("Could not store the installed version!");
		return false;
	}

	m_LocalVersion = m_ClientVersion;
	return true;
}

void Client::MessageLoop()
{
	for (;;)
	{
		static Byte BUF[65536];

		Core::addr_t addr = {};
		int32 len = m_Socket->Recv(BUF, sizeof(BUF), &addr);
		if (len <= 0)
		{
			// Nothing received (0), or an error (-1).
			++m_CurrentRecvAttempt;
			return;
		}

		m_CurrentRecvAttempt = 0;

		// ignore all messages from unknown senders
		if (addr.Value != m_Host.Value)
		{
			continue;
		}

		if (len < sizeof(header_t))
		{
			continue;
		}

		header_t *header = (header_t*)BUF;
		if (header->Type == MessageType::SERVER_RECEIVE_VERSION)
		{
			if (len != sizeof(ServerVersionInfoMessage))
			{
				CAM_LOG_ERROR("Unexpected network message size!");
				continue;
			}

			ServerVersionInfoMessage *msg = (ServerVersionInfoMessage *)BUF;
			CAM_LOG_DEBUG("Received new server version: {}", msg->Version);
			if (msg->Version != m_LocalVersion)
			{
				// Servers of older versions send the key with garbage behind it, so it is compared in its normalized form.
				Core::Crypto::key_t received_key = msg->PublicKey;
				if (received_key.Size <= sizeof(received_key.Data))
				{
					Core::Crypto::NormalizeKey(&received_key);
				}

				if (m_KeyPinned)
				{
					// Only the pinned key is trusted, whatever the server sends.
					if (received_key.Size != m_Config.PublicKey.Size || memcmp(received_key.Data, m_Config.PublicKey.Data, m_Config.PublicKey.Size) != 0)
					{
						CAM_LOG_WARN("The public key of the server differs from the pinned key in {}. Only updates signed with the pinned key are accepted.", m_Config.PublicKeyPath);
					}
				}
				else
				{
					// Trust the key of the server once. It is checked, that it fits into the buffer, the size comes from the network.
					if (received_key.Size == 0 || received_key.Size > sizeof(received_key.Data))
					{
						CAM_LOG_ERROR("The server sent an invalid public key size: {}", msg->PublicKey.Size);
						continue;
					}

					m_Config.PublicKey = received_key;
				}

				// The versions are different, we need an update
				m_Status.Code = ClientStatusCode::NEEDS_UPDATE;
				m_ClientVersion = msg->Version;

				// reset the update state, so that in the next update the update will start
				m_IsFinished = false;
				CAM_LOG_WARN("Requiring update...");
			}
			else
			{
				// The versions are the same, we have the latest version
				m_Status.Code = ClientStatusCode::UP_TO_DATE;
				CAM_LOG_INFO("Binaries are up-to-date. No action required.");
			}
		}
		else if (header->Type == MessageType::SERVER_UPDATE_BEGIN)
		{
			if (len != sizeof(ServerUpdateBeginMessage))
			{
				CAM_LOG_ERROR("Received wrong package size");
				continue;
			}

			ServerUpdateBeginMessage *msg = (ServerUpdateBeginMessage *)BUF;

			// Verify that the update size is reasonable (<200MB).
			if (msg->UpdateSize == 0 || msg->UpdateSize >= (200 * 1024 * 1024))
			{
				CAM_LOG_ERROR("Update size was very unrealistic! Size: {}", msg->UpdateSize);
				continue;
			}

			// Allocate space for update data.
			if (!m_UpdateData.Alloc(msg->UpdateSize))
			{
				CAM_LOG_ERROR("Could not allocated enough space for update!");
				continue;
			}

			// Allocate space for the piece tracker table.
			if (!m_UpdatePieces.Alloc((msg->UpdateSize + PIECE_BYTES - 1) / PIECE_BYTES))
			{
				CAM_LOG_ERROR("Could not allocated enough space for update!");
				continue;
			}

			m_PieceRequestMS.assign(m_UpdatePieces.Size, 0);
			m_InFlight = 0;

			memcpy(&m_UpdateSignature, &msg->UpdateSignature, sizeof(Signature));
			CAM_LOG_DEBUG("Received update begin request, total size: {}", msg->UpdateSize);

			m_Status.Bytes = 0;
			m_Status.Total = m_UpdateData.Size;

			m_IsUpdating = true;
			m_IsFinished = false;
		}
		else if (header->Type == MessageType::SERVER_UPDATE_PIECE)
		{
			if (!m_IsUpdating)
			{
				// Pieces, which were still on their way, when the download was restarted or finished.
				continue;
			}

			ServerUpdatePieceMessage *msg = (ServerUpdatePieceMessage *)BUF;

			// Verify that the tokens match.
			if (msg->ClientToken != m_ClientToken || msg->ServerToken != m_ServerToken)
			{
				CAM_LOG_ERROR("Client or server token did not match!");
				continue;
			}

			// Verify that the message piece size is valid.
			if (msg->PieceSize > PIECE_BYTES)
			{
				CAM_LOG_ERROR("Unexpected message piece size! Expected {0}, but got {1}", PIECE_BYTES, msg->PieceSize);
				continue;
			}

			// Verify that the message contains the full piece data.
			if (len != (sizeof(ServerUpdatePieceMessage) + msg->PieceSize))
			{
				CAM_LOG_ERROR("The network package has not the expected size!");
				continue;
			}

			// Verify that the position is on a piece boundry and something we actually requested.
			if ((msg->PiecePos % PIECE_BYTES) != 0)
			{
				CAM_LOG_ERROR("The piece position {0} is not on a piece boundary of {1} bytes!", msg->PiecePos, PIECE_BYTES);
				continue;
			}

			// Verify that the data doesn't write outside the buffer.
			if (msg->PiecePos + msg->PieceSize > m_UpdateData.Size)
			{
				CAM_LOG_ERROR("The piece pos offset is larger than the update size!");
				continue;
			}

			// Verify the piece position.
			uint32 idx = msg->PiecePos / PIECE_BYTES;
			if (idx >= m_UpdatePieces.Size)
			{
				CAM_LOG_ERROR("The piece index is larger than the update size!");
				continue;
			}

			// Verify that we need the piece.
			if (m_UpdatePieces.Ptr[idx])
			{
				// Happens, if a piece was requested again, while the first answer was still on its way. Not an error, and too frequent to log.
				continue;
			}

			// Validate that the piece size aligns with how we request data.
			if (idx < m_UpdatePieces.Size - 1)
			{
				if (msg->PieceSize != PIECE_BYTES)
				{
					CAM_LOG_ERROR("Piece {0} has the size {1}, but only the last piece may be smaller than {2} bytes!", idx, msg->PieceSize, PIECE_BYTES);
					continue;
				}
			}

			// Copy the update piece into the target buffer.
			memcpy(m_UpdateData.Ptr + msg->PiecePos, BUF + sizeof(ServerUpdatePieceMessage), msg->PieceSize);
			m_UpdatePieces.Ptr[idx] = 1;
			m_Status.Bytes += msg->PieceSize;

			// The piece is not in flight anymore, there is room in the window for the next one.
			if (m_PieceRequestMS[idx] != 0 && m_InFlight > 0)
			{
				--m_InFlight;
			}
		}
		else if (header->Type == MessageType::SERVER_UPDATE_CHANGED)
		{
			if (len != sizeof(ServerUpdateChangedMessage))
			{
				CAM_LOG_ERROR("The update changed message has the size {0}, but {1} was expected!", len, sizeof(ServerUpdateChangedMessage));
				continue;
			}

			ServerUpdateChangedMessage *msg = (ServerUpdateChangedMessage *)BUF;
			if (!m_IsUpdating || msg->ClientToken != m_ClientToken || msg->ServerToken != m_ServerToken)
			{
				// For example a second message, the first one already restarted the download.
				continue;
			}

			if (m_UpdateRestarts >= MAX_UPDATE_RESTARTS)
			{
				CAM_LOG_WARN("The update was replaced on the server again, but the download was restarted {} times already. Not restarting.", m_UpdateRestarts);
				continue;
			}

			++m_UpdateRestarts;
			CAM_LOG_WARN("The update was replaced on the server during the download. Starting again with the new update ({0} of {1})...", m_UpdateRestarts, MAX_UPDATE_RESTARTS);
			RestartDownload();
		}
		else if (header->Type == MessageType::SERVER_UPDATE_TOKEN)
		{
			if (len != sizeof(ServerUpdateTokenMessage))
			{
				CAM_LOG_ERROR("The token message has the size {0}, but {1} was expected!", len, sizeof(ServerUpdateTokenMessage));
				continue;
			}

			ServerUpdateTokenMessage *msg = (ServerUpdateTokenMessage *)BUF;
			if (msg->ClientToken != m_ClientToken)
			{
				CAM_LOG_ERROR("Client tokens did not match!");
				continue;
			}

			CAM_LOG_INFO("Received new server token: {}", msg->ServerToken);
			m_ServerToken = msg->ServerToken;
		}
	}
}

uint32 Client::ReadInstalledVersion()
{
	std::string file = m_Config.UpdateBinaryPath + "/" + INSTALLED_VERSION_FILE;
	std::string content;
	if (!Core::FileSystem::Get()->FileExists(file) || Core::FileSystem::Get()->ReadTextFile(file, &content) == 0)
	{
		return 0;
	}

	try
	{
		int value = std::stoi(content);
		return value > 0 ? (uint32)value : 0;
	}
	catch (const std::exception &)
	{
		CAM_LOG_ERROR("The file {} does not contain a valid version.", file);
		return 0;
	}
}

bool Client::LoadLocalVersion()
{
	// The installed update wins, the version in the source code never changes by installing an update.
	uint32 installed_version = ReadInstalledVersion();
	if (installed_version)
	{
		m_LocalVersion = installed_version;
		return true;
	}

	// Otherwise load the version of the source.
	uint32 local_version = Core::utils::GetLocalVersion(m_Config.UpdateTargetPath);
	if (!local_version)
	{
		return false;
	}

	m_LocalVersion = local_version;
	return true;
}

void Client::LoadPinnedKey()
{
	Core::FileSystem *fs = Core::FileSystem::Get();
	if (m_Config.PublicKeyPath.empty() || !fs->FileExists(m_Config.PublicKeyPath))
	{
		return;
	}

	uint32 size = 0;
	Byte *data = fs->ReadFile(m_Config.PublicKeyPath, &size);
	if (!data)
	{
		CAM_LOG_ERROR("Could not read the pinned public key {}!", m_Config.PublicKeyPath);
		return;
	}

	if (size > 0 && size <= sizeof(m_Config.PublicKey.Data))
	{
		memset(m_Config.PublicKey.Data, 0, sizeof(m_Config.PublicKey.Data));
		m_Config.PublicKey.Size = size;
		memcpy(m_Config.PublicKey.Data, data, size);

		// Pinned keys of older versions have garbage behind the key.
		Core::Crypto::NormalizeKey(&m_Config.PublicKey);
		m_KeyPinned = true;
	}
	else
	{
		CAM_LOG_ERROR("The pinned public key {} has an invalid size!", m_Config.PublicKeyPath);
	}

	delete[] data;
}

void Client::RestartDownload()
{
	// What was received belongs to the old update. The new one may also have another version (and key), so the version is asked for again.
	Reset();
	RequestServerVersion();
}

void Client::Reset()
{
	m_UpdateData.Free();
	m_UpdatePieces.Free();
	m_PieceRequestMS.clear();
	m_InFlight = 0;

	m_IsFinished = true;
	m_IsUpdating = false;

	m_ClientToken = 0;
	m_ServerToken = 0;

	m_UpdateIdx = 0;
}

void Client::UpdateProgress(int64 now_ms, Core::addr_t addr)
{
	if (m_IsFinished)
	{
		return;
	}

	if (!m_IsUpdating)
	{
		if (now_ms - m_LastUpdateMS >= 1000)
		{
			m_LastUpdateMS = now_ms;
			m_ClientToken = m_Crypto->GenToken();

			// Send update begin request to server
			CAM_LOG_DEBUG("Sending update begin request...");
			ClientUpdateBeginMessage begin_update = {};
			begin_update.Header.Type = MessageType::CLIENT_UPDATE_BEGIN;
			begin_update.Header.Version = m_ClientVersion;
			begin_update.ClientVersion = m_ClientVersion;
			begin_update.ClientToken = m_ClientToken;
			begin_update.ServerToken = m_ServerToken;
			m_Socket->Send(&begin_update, sizeof(begin_update), m_Host);
			m_Status.Code = ClientStatusCode::NONE;
		}

		return;
	}

	// We are updating
	if (now_ms - m_LastProgressLogMS >= 1000)
	{
		m_LastProgressLogMS = now_ms;
		CAM_LOG_INFO("Downloading update: {0:.1f} / {1:.1f} MB ({2}%)", m_Status.Bytes / 1048576.0, m_Status.Total / 1048576.0, m_Status.Total ? (uint64)m_Status.Bytes * 100 / m_Status.Total : 0);
	}

	if (m_UpdateIdx >= m_UpdatePieces.Size)
	{
		if (m_Crypto->TestSignature(m_UpdateSignature.Data, SIG_BYTES, m_UpdateData.Ptr, m_UpdateData.Size, m_Config.PublicKey.Data, m_Config.PublicKey.Size))
		{
			if (!m_KeyPinned)
			{
				// The first update was verified with the key of the server. From now on, only this key is trusted.
				if (ReplaceFile(m_Config.PublicKeyPath, m_Config.PublicKey.Data, m_Config.PublicKey.Size))
				{
					m_KeyPinned = true;
					CAM_LOG_INFO("Pinned the public key of the server in {}.", m_Config.PublicKeyPath);
				}
				else
				{
					CAM_LOG_WARN("Could not store the public key in {}, it is not pinned.", m_Config.PublicKeyPath);
				}
			}

			std::string update_file = m_Config.UpdateBinaryPath + "/update.zip";
			CAM_LOG_DEBUG("Writing file {}", update_file);

			bool writeSuccess = Core::FileSystem::Get()->MakeDirectory(m_Config.UpdateBinaryPath)
				&& ReplaceFile(update_file, m_UpdateData.Ptr, m_UpdateData.Size);

			m_IsFinished = true;
			m_IsUpdating = false;
			m_UpdateData.Free();
			m_UpdatePieces.Free();

			if (writeSuccess)
			{
				m_Status.Code = ClientStatusCode::DOWNLOADED;
				CAM_LOG_INFO("File {} written successfully.", update_file);
			}
			else
			{
				m_Status.Code = ClientStatusCode::BAD_WRITE;
				CAM_LOG_ERROR("Could not write the file {}!", update_file);
			}

			return;
		}
		else
		{
			m_Status.Code = ClientStatusCode::BAD_SIG;
			CAM_LOG_ERROR("The signature of the update does not match the {} public key.", m_KeyPinned ? "pinned" : "server's");
			if (m_KeyPinned)
			{
				CAM_LOG_ERROR("If the key of the update server was changed on purpose, delete {} on this client to trust the new key.", m_Config.PublicKeyPath);
			}
		}

		Reset();
		return;
	}

	// Update in progress, request pieces.
	if (now_ms - m_LastPieceMS >= 10)
	{
		m_LastPieceMS = now_ms;

		ClientUpdatePieceMessage msg = {};
		msg.Header.Version = m_LocalVersion;
		msg.Header.Type = MessageType::CLIENT_UPDATE_PIECE;
		msg.ClientToken = m_ClientToken;
		msg.ServerToken = m_ServerToken;

		bool found_missing = false;
		uint32 first_missing = 0;
		uint32 num_requests = 0;

		for (uint32 idx = m_UpdateIdx, end = m_UpdatePieces.Size; idx < end; ++idx)
		{
			if (m_UpdatePieces.Ptr[idx])
			{
				continue;
			}

			// Keep track of the first piece, which is still missing.
			if (!found_missing)
			{
				found_missing = true;
				first_missing = idx;
			}

			if (m_PieceRequestMS[idx] != 0)
			{
				// Requested before. Only if the answer did not arrive in time (the request or the answer got lost, or the server dropped it), it is requested again.
				// It is still counted as in flight, so this does not use up room of the window.
				if (now_ms - m_PieceRequestMS[idx] < PIECE_RETRY_MS)
				{
					continue;
				}
			}
			else
			{
				// A new piece, which needs room in the window.
				if (m_InFlight >= REQUEST_WINDOW)
				{
					break;
				}

				++m_InFlight;
			}

			msg.PiecePos = idx * PIECE_BYTES;
			m_Socket->Send(&msg, sizeof(msg), m_Host);
			m_PieceRequestMS[idx] = now_ms;

			if (++num_requests >= MAX_REQUESTS)
			{
				break;
			}
		}

		if (found_missing)
		{
			m_UpdateIdx = first_missing;
		}
		else
		{
			// Nothing is missing anymore, the next call checks the signature.
			m_UpdateIdx = m_UpdatePieces.Size;
			CAM_LOG_INFO("Received all update pieces successfully.");
		}
	}
}
