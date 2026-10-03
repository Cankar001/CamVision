#include "Server.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <filesystem>

#include "Utils/Utils.h"
#include "Utils/ZipArchive.h"
#include "Core/Log.h"

Server::Server(const ServerConfig &config)
	: m_Config(config)
{
	m_LastUpdateCheckMS = 0;
	m_LastUpdateWriteMS = 0;
	m_UpdateSignature = {};

	m_Socket = Core::Socket::Create();
	m_Crypto = Core::Crypto::Create();
	m_IPTable = new Core::IPTable();
	m_Clients = new Core::Clients(m_Crypto, m_IPTable);
	Core::Clients::SetSpeedLimit(m_Config.ClientSpeedLimitKB * 1000);

	m_LocalVersion = ResolveVersion();
	if (m_LocalVersion == 0)
	{
		CAM_LOG_ERROR("Could not determine the version! Put a version.txt into {0}, fix the source path {1} (CamVersion.h), or set the version explicitly.", m_Config.TargetBinaryPath, m_Config.TargetSourcePath);
	}

	std::string cwd = "";
	Core::FileSystem::Get()->GetCurrentWorkingDirectory(&cwd);

	CAM_LOG_INFO("===================== CONFIG ===================================");
	CAM_LOG_INFO("Private key path      : {}", config.PrivateKeyPath);
	CAM_LOG_INFO("Public key path       : {}", config.PublicKeyPath);
	CAM_LOG_INFO("IP                    : {}", config.ServerIP);
	CAM_LOG_INFO("Port                  : {}", config.ServerPort);
	CAM_LOG_INFO("Speed limit per client: {}", config.ClientSpeedLimitKB == 0 ? std::string("none") : std::to_string(config.ClientSpeedLimitKB) + " KB/s");
	CAM_LOG_INFO("Signature path        : {}", config.SignaturePath);
	CAM_LOG_INFO("Target binary path    : {}", config.TargetBinaryPath);
	CAM_LOG_INFO("Target source path    : {}", config.TargetSourcePath);
	CAM_LOG_INFO("Current Server version: {}", m_LocalVersion);
	CAM_LOG_INFO("Current CWD           : {}", cwd);
	CAM_LOG_INFO("================================================================");
}

Server::~Server()
{
	m_WatchRunning = false;
	if (m_WatchThread.joinable())
	{
		m_WatchThread.join();
	}

	delete m_Clients;
	m_Clients = nullptr;

	delete m_IPTable;
	m_IPTable = nullptr;

	delete m_Crypto;
	m_Crypto = nullptr;

	delete m_Socket;
	m_Socket = nullptr;
}

void Server::Run()
{
	CAM_LOG_INFO("Waiting for clients to connect...");

	for (;;)
	{
		m_Socket->Open();

		// Many clients request pieces at the same time, the requests must not get lost, because the socket buffer is full.
		m_Socket->SetBufferSizes(4 * 1024 * 1024, 4 * 1024 * 1024);

		if (!m_Socket->Bind(m_Config.ServerPort))
		{
			CAM_LOG_ERROR("Could not bind socket");
			break;
		}

		for (;;)
		{
			if (!Step())
			{
				break;
			}
		}

		CAM_LOG_ERROR("network interface failure.");
		m_Socket->Close();

		Core::SleepMS(10);
	}

	CAM_LOG_ERROR("socket creation failed.");
}

uint32 Server::ResolveVersion() const
{
	if (m_Config.Version != 0)
	{
		return m_Config.Version;
	}

	// A version.txt in the binary folder travels together with the binaries, so a folder can be replaced without any source code on the server.
	Core::FileSystem *fs = Core::FileSystem::Get();
	std::string version_file = m_Config.TargetBinaryPath + "/version.txt";
	std::string content;
	if (fs->FileExists(version_file) && fs->ReadTextFile(version_file, &content) != 0)
	{
		try
		{
			int version = std::stoi(content);
			if (version > 0)
			{
				return (uint32)version;
			}
		}
		catch (const std::exception &)
		{
		}

		CAM_LOG_ERROR("{} does not contain a valid version, ignoring it.", version_file);
	}

	return Core::utils::GetLocalVersion(m_Config.TargetSourcePath);
}

bool Server::ShouldShip(const std::filesystem::directory_entry &entry, bool skipDebugFiles) const
{
	std::error_code error;
	if (!entry.is_regular_file(error) || error)
	{
		// The update contains no folders.
		return false;
	}

	std::string name = entry.path().filename().string();

	// The private key must never be shipped to the clients, even if the key is stored in the folder, which is shipped.
	if (name == std::filesystem::path(m_Config.PrivateKeyPath).filename().string())
	{
		return false;
	}

	// skip the archive itself (only the name matters, the path to it may contain anything)
	if (name.find("update") != std::string::npos)
	{
		return false;
	}

	if (skipDebugFiles && name.find(".pdb") != std::string::npos)
	{
		return false;
	}

	return true;
}

std::string Server::ComputeFingerprint() const
{
	// The folder may disappear or change at any moment, so only the error code variants are used, which never throw.
	std::error_code error;
	std::filesystem::directory_iterator it(m_Config.TargetBinaryPath, error);
	if (error)
	{
		return "";
	}

	std::vector<std::string> entries;
	for (; !error && it != std::filesystem::directory_iterator(); it.increment(error))
	{
		if (!ShouldShip(*it))
		{
			continue;
		}

		std::error_code size_error, time_error;
		uint64 size = (uint64)it->file_size(size_error);
		auto time = it->last_write_time(time_error);
		if (size_error || time_error)
		{
			// The file is just being replaced.
			continue;
		}

		entries.push_back(it->path().filename().string() + "|" + std::to_string(size) + "|" + std::to_string((int64)time.time_since_epoch().count()));
	}

	std::sort(entries.begin(), entries.end());

	std::string fingerprint;
	for (const std::string &entry : entries)
	{
		fingerprint += entry;
		fingerprint += '\n';
	}

	return fingerprint;
}

// Reads a key file. The size comes from the disk, so it is checked that it fits.
static bool ReadKeyFile(const std::string &path, Core::Crypto::key_t *out_key, uint32 *out_file_size)
{
	uint32 size = 0;
	Byte *data = Core::FileSystem::Get()->ReadFile(path, &size);
	if (!data)
	{
		CAM_LOG_ERROR("Could not read the key file {}!", path);
		return false;
	}

	bool success = size > 0 && size <= sizeof(out_key->Data);
	if (!success)
	{
		CAM_LOG_ERROR("The key file {0} has an invalid size of {1} bytes!", path, size);
	}

	if (success)
	{
		memset(out_key->Data, 0, sizeof(out_key->Data));
		out_key->Size = size;
		memcpy(out_key->Data, data, size);
		*out_file_size = size;

		// Old key files contain uninitialized memory behind the key.
		Core::Crypto::NormalizeKey(out_key);
	}

	delete[] data;
	return success;
}

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

// The private key allows to sign updates for all clients, nobody but the owner may read it.
static bool WritePrivateKeyFile(const std::string &path, const void *data, uint32 size)
{
	if (!ReplaceFile(path, data, size))
	{
		return false;
	}

#ifndef _WIN32
	chmod(path.c_str(), 0600);
#endif

	return true;
}

bool Server::LoadUpdateFile(bool regenerateKeys, bool skipDebugFiles)
{
	// The new update is built on the side and only replaces the current one at the very end. If anything fails, the current update stays available,
	// and the network thread is never blocked while the (possibly large) update is packed.
	Core::FileSystem *fs = Core::FileSystem::Get();

	// The version may have changed since the start (new build, other folder).
	uint32 version = ResolveVersion();
	if (version == 0)
	{
		CAM_LOG_ERROR("Could not determine the version of the update!");
		return false;
	}

	// First check, if the update path is valid
	std::string update_path = m_Config.TargetBinaryPath;
	std::string update_file = update_path + "/update.zip";

	if (!fs->DirectoryExists(update_path))
	{
		CAM_LOG_ERROR("Binary path from source does not exist! Please re-check your binary path or build the source first.");
		return false;
	}

	CAM_LOG_INFO("Generating new update package at location {} ...", update_file);

	// then delete an existing update file
	if (fs->FileExists(update_file))
	{
		if (!fs->RemoveFile(update_file))
		{
			CAM_LOG_ERROR("Could not delete the file {}", update_file);
			return false;
		}
		else
		{
			CAM_LOG_INFO("Deleted existing packaged update from disk.");
		}
	}

	// load the contents of the directory and store them into the zip file
	std::vector<Core::ZipFile> files;
	auto free_files = [&files]()
	{
		for (auto &file : files)
		{
			delete[] (Byte *)file.Buffer;
			file.Buffer = nullptr;
		}
	};

	std::error_code iterator_error;
	std::filesystem::directory_iterator iterator(update_path, iterator_error);
	if (iterator_error)
	{
		CAM_LOG_ERROR("Could not read the folder {}: {}", update_path, iterator_error.message());
		return false;
	}

	for (const std::filesystem::directory_entry &entry : iterator)
	{
		const std::filesystem::path p = entry.path();
		std::string zip_name = p.filename().string();
		std::string current_file_name = p.string();

		if (!ShouldShip(entry, skipDebugFiles))
		{
			CAM_LOG_INFO("Skipping {}.", zip_name);
			continue;
		}

		CAM_LOG_INFO("Adding {}...", zip_name);

		uint32 file_size = 0;
		Byte *data = fs->ReadFile(current_file_name, &file_size);
		if (!data)
		{
			CAM_LOG_ERROR("Could not read file {}!", current_file_name);
			free_files();
			return false;
		}

		Core::ZipFile file;
		file.Name = zip_name;
		file.Path = current_file_name;
		file.Buffer = data;
		file.BufferSize = file_size;
		files.push_back(file);
	}

	if (files.empty())
	{
		CAM_LOG_ERROR("There are no files to ship in {}!", update_path);
		return false;
	}

	CAM_LOG_INFO("Added all files.");

	// Now trying to store the whole contents into the zip file
	CAM_LOG_INFO("Writing zip file to memory...");
	Core::ZipArchive archive;
	bool stored = archive.Store(files, update_file);

	// Cleanup the memory.
	free_files();

	if (!stored)
	{
		CAM_LOG_ERROR("Could not save the zip file!");
		return false;
	}

	CAM_LOG_INFO("Zip file written successfully to {}", update_file);

	// Load the whole ZIP file into memory
	Core::FileSystemBuffer new_update_file;
	new_update_file.Data = fs->ReadFile(update_file, &new_update_file.Size);

	if (!new_update_file.Data)
	{
		CAM_LOG_ERROR("Could not read back in the update file!");
		return false;
	}

	if (regenerateKeys)
	{
		for (const std::string *path : { &m_Config.PrivateKeyPath, &m_Config.PublicKeyPath, &m_Config.SignaturePath })
		{
			if (fs->FileExists(*path) && !fs->RemoveFile(*path))
			{
				CAM_LOG_ERROR("Could not remove {}!", *path);
				return false;
			}
		}
	}

	// The key pair stays the same for all updates, clients pin the public key. A new pair is only generated, if there is none (or if it is damaged).
	Core::Crypto::key_t private_key, public_key;
	bool keys_generated = false;
	bool keys_loaded = false;
	if (fs->FileExists(m_Config.PrivateKeyPath) && fs->FileExists(m_Config.PublicKeyPath))
	{
		uint32 private_file_size = 0, public_file_size = 0;
		keys_loaded = ReadKeyFile(m_Config.PrivateKeyPath, &private_key, &private_file_size) && ReadKeyFile(m_Config.PublicKeyPath, &public_key, &public_file_size);
		if (!keys_loaded)
		{
			CAM_LOG_ERROR("The existing key files could not be read, generating a new key pair!");
		}
		else
		{
			// Rewrite key files, which were stored with garbage behind the key (created by older versions).
			if (private_key.Size != private_file_size)
			{
				CAM_LOG_INFO("Cleaning up the private key file {}.", m_Config.PrivateKeyPath);
				WritePrivateKeyFile(m_Config.PrivateKeyPath, private_key.Data, private_key.Size);
			}

			if (public_key.Size != public_file_size)
			{
				CAM_LOG_INFO("Cleaning up the public key file {}.", m_Config.PublicKeyPath);
				ReplaceFile(m_Config.PublicKeyPath, public_key.Data, public_key.Size);
			}
		}
	}

	if (!keys_loaded)
	{
		if (!m_Crypto->GenKeys(&public_key, &private_key))
		{
			CAM_LOG_ERROR("Could not generate public/private key pair!");
			return false;
		}

		keys_generated = true;
		CAM_LOG_INFO("Generated a new public/private key pair.");
	}

	// make the signature for the file
	// The signature covers the version too, so an old update cannot be offered under a newer version.
	std::vector<Byte> signed_data = Core::utils::BuildSignedUpdateData(version, new_update_file.Data, new_update_file.Size);
	Signature new_signature = {};
	if (!m_Crypto->SignSignature(
		new_signature.Data,
		sizeof(new_signature.Data),
		signed_data.data(),
		(uint32)signed_data.size(),
		private_key.Data,
		private_key.Size))
	{
		CAM_LOG_ERROR("Could not sign the update!");
		return false;
	}

	// Now write the security files
	if (keys_generated)
	{
		if (!WritePrivateKeyFile(m_Config.PrivateKeyPath, private_key.Data, private_key.Size))
		{
			CAM_LOG_ERROR("Could not write private key file!");
			return false;
		}

		if (!ReplaceFile(m_Config.PublicKeyPath, public_key.Data, public_key.Size))
		{
			CAM_LOG_ERROR("Could not write public key file!");
			return false;
		}
	}

	if (!ReplaceFile(m_Config.SignaturePath, new_signature.Data, SIG_BYTES))
	{
		CAM_LOG_ERROR("Could not write the new signature!");
		return false;
	}

	// Everything worked, now the new update replaces the old one. This is the only moment, where the network thread has to wait.
	uint32 new_size = new_update_file.Size;
	uint64 new_update_id = Core::Raw64(new_update_file.Data, new_update_file.Size);
	if (new_update_id == 0)
	{
		new_update_id = 1;
	}

	{
		std::lock_guard<std::mutex> lock(m_UpdateMutex);

		std::swap(m_UpdateFile.Data, new_update_file.Data);
		std::swap(m_UpdateFile.Size, new_update_file.Size);
		m_UpdateSignature = new_signature;
		m_UpdateId = new_update_id;
		m_PublicKey = public_key;
		m_LocalVersion = version;
	}

	// new_update_file now holds the old update, which is released when it goes out of scope.
	CAM_LOG_INFO("Loaded update with size {0}, version {1}", new_size, version);

	return true;
}

void Server::StartFileWatcher()
{
	// The update, which was built before, matches the current state of the folder.
	m_BuiltFingerprint = ComputeFingerprint();

	m_WatchRunning = true;
	m_WatchThread = std::thread(&Server::WatchLoop, this);
}

void Server::WatchLoop()
{
	const int64 poll_ms = 1000;
	const int64 settle_ms = 3000;	// The changes must not change for this long, before the update is rebuilt.
	const int64 retry_ms = 10000;	// Pause after a failed build.

	std::string pending;
	bool has_pending = false;
	int64 pending_since = 0;
	int64 next_attempt_ms = 0;

	while (m_WatchRunning)
	{
		// Sleep in small steps, so the server can stop quickly.
		for (int64 waited = 0; waited < poll_ms && m_WatchRunning; waited += 100)
		{
			Core::SleepMS(100);
		}

		std::string fingerprint = ComputeFingerprint();
		if (fingerprint == m_BuiltFingerprint)
		{
			has_pending = false;
			continue;
		}

		int64 now = Core::QueryMS();
		if (!has_pending || fingerprint != pending)
		{
			// Something changed (again), wait until it stays the same for a while.
			pending = fingerprint;
			has_pending = true;
			pending_since = now;

			if (fingerprint.empty())
			{
				CAM_LOG_WARN("The folder {} is missing or has no files. The current update stays available.", m_Config.TargetBinaryPath);
			}
			else
			{
				CAM_LOG_INFO("Changes in {} detected, waiting until the files are stable...", m_Config.TargetBinaryPath);
			}

			continue;
		}

		if (fingerprint.empty() || now - pending_since < settle_ms || now < next_attempt_ms)
		{
			continue;
		}

		CAM_LOG_INFO("The files are stable, building the new update...");

		uint32 previous_version = 0;
		{
			std::lock_guard<std::mutex> lock(m_UpdateMutex);
			previous_version = m_LocalVersion;
		}

		if (LoadUpdateFile())
		{
			// The fingerprint was taken before the build. If files changed while building, the next poll sees a difference and builds again.
			m_BuiltFingerprint = fingerprint;
			has_pending = false;

			uint32 new_version = 0;
			{
				std::lock_guard<std::mutex> lock(m_UpdateMutex);
				new_version = m_LocalVersion;
			}

			if (new_version == previous_version)
			{
				CAM_LOG_WARN("The files changed, but the version is still {}. Clients with this version do not update! Increase CAM_VERSION or the number in version.txt.", new_version);
			}
		}
		else
		{
			next_attempt_ms = now + retry_ms;
			CAM_LOG_ERROR("Could not build the new update. The previous update stays available, trying again in {} seconds.", retry_ms / 1000);
		}
	}
}

namespace
{
	// "192.168.1.20:51234" (the address is stored in network order, the bytes are read one by one, so it works on every machine).
	std::string FormatAddress(Core::addr_t addr)
	{
		const Byte *host = (const Byte *)&addr.Host;
		const Byte *port = (const Byte *)&addr.Port;
		return std::to_string(host[0]) + "." + std::to_string(host[1]) + "." + std::to_string(host[2]) + "." + std::to_string(host[3]) + ":" + std::to_string((port[0] << 8) | port[1]);
	}

	double Megabytes(uint32 bytes)
	{
		return bytes / 1048576.0;
	}
}

void Server::BeginTransfer(Core::addr_t addr, uint64 clientToken, int64 now_ms)
{
	// The client asks for the begin of the update again, if our answer got lost: not a new transfer.
	auto existing = m_Transfers.find(addr.Value);
	if (existing != m_Transfers.end() && existing->second.ClientToken == clientToken)
	{
		existing->second.LastActivityMS = now_ms;
		return;
	}

	Transfer transfer;
	transfer.ClientToken = clientToken;
	transfer.Version = m_LocalVersion;
	transfer.TotalBytes = m_UpdateFile.Size;
	transfer.Served.assign((m_UpdateFile.Size + PIECE_BYTES - 1) / PIECE_BYTES, false);
	transfer.StartMS = now_ms;
	transfer.LastLogMS = now_ms;
	transfer.LastActivityMS = now_ms;
	m_Transfers[addr.Value] = std::move(transfer);

	CAM_LOG_INFO("Sending update version {0} ({1:.1f} MB) to {2}...", m_LocalVersion, Megabytes(m_UpdateFile.Size), FormatAddress(addr));
}

void Server::PieceSent(Core::addr_t addr, uint64 clientToken, uint32 piecePos, uint32 pieceSize, int64 now_ms)
{
	auto found = m_Transfers.find(addr.Value);
	if (found == m_Transfers.end() || found->second.ClientToken != clientToken)
	{
		// The begin was not seen: start the log now.
		BeginTransfer(addr, clientToken, now_ms);
		found = m_Transfers.find(addr.Value);
	}

	Transfer &transfer = found->second;
	transfer.LastActivityMS = now_ms;

	uint32 index = piecePos / PIECE_BYTES;
	if (index < transfer.Served.size() && !transfer.Served[index])
	{
		transfer.Served[index] = true;
		++transfer.ServedPieces;
		transfer.ServedBytes += pieceSize;
	}

	if (transfer.ServedPieces == transfer.Served.size())
	{
		CAM_LOG_INFO("Update version {0} sent completely to {1}: {2:.1f} MB in {3:.1f} s.", transfer.Version, FormatAddress(addr), Megabytes(transfer.TotalBytes), (now_ms - transfer.StartMS) / 1000.0);
		m_Transfers.erase(found);
		return;
	}

	if (now_ms - transfer.LastLogMS >= 1000)
	{
		transfer.LastLogMS = now_ms;
		CAM_LOG_INFO("Sending update to {0}: {1:.1f} / {2:.1f} MB ({3}%)", FormatAddress(addr), Megabytes(transfer.ServedBytes), Megabytes(transfer.TotalBytes), (uint64)transfer.ServedBytes * 100 / transfer.TotalBytes);
	}
}

void Server::ExpireTransfers(int64 now_ms)
{
	if (now_ms - m_LastTransferSweepMS < 5000)
	{
		return;
	}

	m_LastTransferSweepMS = now_ms;
	for (auto it = m_Transfers.begin(); it != m_Transfers.end();)
	{
		const Transfer &transfer = it->second;
		if (now_ms - transfer.LastActivityMS < 15000)
		{
			++it;
			continue;
		}

		Core::addr_t addr = {};
		addr.Value = it->first;
		CAM_LOG_WARN("The client {0} stopped downloading the update at {1:.1f} / {2:.1f} MB ({3}%).", FormatAddress(addr), Megabytes(transfer.ServedBytes), Megabytes(transfer.TotalBytes), (uint64)transfer.ServedBytes * 100 / transfer.TotalBytes);
		it = m_Transfers.erase(it);
	}
}

bool Server::Step()
{
	static Byte BUF[65536];

	Core::addr_t addr;
	int32 len = m_Socket->Recv(BUF, sizeof(BUF), &addr);
	if (len < 0)
	{
		return false;
	}

	if (len < sizeof(header_t))
	{
		return true;
	}

	// The update may be rebuilt by the file watcher at any time.
	std::lock_guard<std::mutex> lock(m_UpdateMutex);

	header_t *header = (header_t *)BUF;
	if (header->Type == MessageType::CLIENT_UPDATE_BEGIN)
	{
		if (len != sizeof(ClientUpdateBeginMessage))
		{
			return true;
		}

		ClientUpdateBeginMessage *msg = (ClientUpdateBeginMessage *)BUF;

		int64 now_ms = Core::QueryMS();
		ExpireTransfers(now_ms);
		Core::Clients::Node *client = m_Clients->Insert(addr, now_ms);

		if (!client)
		{
			CAM_LOG_ERROR("Client could not be inserted!");
			return true;
		}

		if (!client->IsBandwidthAvailable(now_ms))
		{
		//	CAM_LOG_ERROR("Client has no bandwidth available!");
			return true;
		}

		if (msg->ServerToken != client->ServerToken)
		{
			// The normal start of every download: the client does not know its token yet (nothing is logged, it happens for every download).
			ServerUpdateTokenMessage res = {};
			res.Header.Type = MessageType::SERVER_UPDATE_TOKEN;
			res.Header.Version = m_LocalVersion;
			res.ClientToken = msg->ClientToken;
			res.ServerToken = client->ServerToken;
			m_Socket->Send(&res, sizeof(res), addr);

			client->Bandwidth += sizeof(ServerUpdateTokenMessage);

			return true;
		}

		ServerUpdateBeginMessage res;
		memset(&res, 0, sizeof(res));

		res.Header.Version = m_LocalVersion;
		res.Header.Type = MessageType::SERVER_UPDATE_BEGIN;
		res.ClientToken = msg->ClientToken;
		res.ServerToken = client->ServerToken;
		res.UpdateSize = m_UpdateFile.Size;
		res.UpdateSignature = m_UpdateSignature;
		m_Socket->Send(&res, sizeof(res), addr);

		// From now on, the client downloads this update.
		client->UpdateId = m_UpdateId;
		BeginTransfer(addr, msg->ClientToken, now_ms);

		client->Bandwidth += sizeof(ServerUpdateBeginMessage);
	}
	else if (header->Type == MessageType::CLIENT_UPDATE_PIECE)
	{
		if (len != sizeof(ClientUpdatePieceMessage))
		{
			return true;
		}

		int64 now_ms = Core::QueryMS();

		ClientUpdatePieceMessage *msg = (ClientUpdatePieceMessage *)BUF;
		ExpireTransfers(now_ms);

		Core::Clients::Node *client = m_Clients->Insert(addr, now_ms);

		if (!client)
		{
			CAM_LOG_ERROR("Could not find the client for addr {0}, port {1}", addr.Host, addr.Port);
			return true;
		}

		if (!client->IsBandwidthAvailable(now_ms))
		{
		//	CAM_LOG_ERROR("Client has no bandwidth available!");
			return true;
		}

		if (msg->ServerToken != client->ServerToken)
		{
			CAM_LOG_ERROR("Server tokens did not match!");
			return true;
		}

		if (client->UpdateId != m_UpdateId)
		{
			// The update was replaced, while the client downloaded it. The pieces of the new one do not fit to the received ones, the client has to start again.
			// Pieces, which were sent before, are still fine for the client. Every request is answered like this, until the client began again (with a begin
			// message), so pieces of the two updates are never mixed, even if this message gets lost.
			auto transfer = m_Transfers.find(addr.Value);
			if (transfer != m_Transfers.end())
			{
				CAM_LOG_INFO("The update was replaced while {0} downloaded it ({1:.1f} / {2:.1f} MB), the client starts again with the new one.", FormatAddress(addr), Megabytes(transfer->second.ServedBytes), Megabytes(transfer->second.TotalBytes));
				m_Transfers.erase(transfer);
			}

			ServerUpdateChangedMessage changed = {};
			changed.Header.Type = MessageType::SERVER_UPDATE_CHANGED;
			changed.Header.Version = m_LocalVersion;
			changed.ClientToken = msg->ClientToken;
			changed.ServerToken = client->ServerToken;
			m_Socket->Send(&changed, sizeof(changed), addr);
			client->Bandwidth += sizeof(changed);

			return true;
		}

		if (msg->PiecePos >= m_UpdateFile.Size)
		{
			CAM_LOG_ERROR("The request position was larger than the file!");
			return true;
		}

		ServerUpdatePieceMessage res = {};
		res.Header.Version = m_LocalVersion;
		res.Header.Type = MessageType::SERVER_UPDATE_PIECE;
		res.ClientToken = msg->ClientToken;
		res.ServerToken = client->ServerToken;
		res.PiecePos = msg->PiecePos;
		res.PieceSize = (uint16)Core::utils::Min<uint32>(m_UpdateFile.Size - msg->PiecePos, PIECE_BYTES);

		// A piece is never larger than PIECE_BYTES, so no allocation is needed for every piece.
		char send_buf[sizeof(ServerUpdatePieceMessage) + PIECE_BYTES];
		memcpy(send_buf, &res, sizeof(res));
		memcpy(send_buf + sizeof(res), m_UpdateFile.Data + msg->PiecePos, res.PieceSize);

		uint32 send_bytes = sizeof(res) + res.PieceSize;
		m_Socket->Send(send_buf, send_bytes, addr);
		client->Bandwidth += send_bytes;

		PieceSent(addr, msg->ClientToken, msg->PiecePos, res.PieceSize, now_ms);
	}
	else if (header->Type == MessageType::CLIENT_REQUEST_VERSION)
	{
		if (len != sizeof(ClientWantsVersionMessage))
		{
			CAM_LOG_ERROR("ClientWantsVersionMessage: Unexpected message size.");
			return true;
		}

		ClientWantsVersionMessage *msg = (ClientWantsVersionMessage *)BUF;
		uint32 client_version = msg->LocalVersion;

		ServerVersionInfoMessage res = {};
		res.Header.Type = MessageType::SERVER_RECEIVE_VERSION;
		res.Header.Version = m_LocalVersion;
		// Without an update there is nothing to offer, so the client is told, that it is up to date. The same if the update is not newer than the one of the
		// client: updates are never offered as a downgrade (clients refuse them anyway). To roll back, publish the old binaries under a higher version.
		res.Version = (m_UpdateFile.Size != 0 && m_LocalVersion > client_version) ? m_LocalVersion : client_version;
		res.PublicKey.Size = m_PublicKey.Size;
		memcpy(res.PublicKey.Data, m_PublicKey.Data, m_PublicKey.Size);
		m_Socket->Send(&res, sizeof(res), addr);
	}

	return true;
}

