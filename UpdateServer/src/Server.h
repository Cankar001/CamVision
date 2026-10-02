#pragma once

#include <Cam-Core.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "Message.h"

struct ServerConfig
{
	/// <summary>
	/// Contains the path to the root path of the source, to read the version.
	/// </summary>
	std::string TargetSourcePath;

	/// <summary>
	/// Contains the path to the binaries from the source, which should be shipped to the clients.
	/// </summary>
	std::string TargetBinaryPath;

	/// <summary>
	/// The IP v4 address of the server (informational, the server listens on all interfaces)
	/// </summary>
	std::string ServerIP = "0.0.0.0";

	/// <summary>
	/// The port of the server
	/// </summary>
	uint16 ServerPort = 44200;

	/// <summary>
	/// The version, which is announced to the clients. 0 reads the version from the source (CamVersion.h in TargetSourcePath).
	/// </summary>
	uint32 Version = 0;

	/// <summary>
	/// The maximum speed in KB/s, with which an update is sent to a single client. The clients request what they need, the server drops requests over this limit.
	/// 0 means no limit.
	/// </summary>
	uint32 ClientSpeedLimitKB = 10000;

	/// <summary>
	/// the path to the public key
	/// </summary>
	std::string PublicKeyPath;

	/// <summary>
	/// The path to the private key
	/// </summary>
	std::string PrivateKeyPath;

	/// <summary>
	/// The path to the signature
	/// </summary>
	std::string SignaturePath;
};

class Server
{
public:

	Server(const ServerConfig &config);
	~Server();

	/// <summary>
	/// Runs the server.
	/// </summary>
	void Run();

	/// <summary>
	/// The version, which is announced to the clients. 0, if it could not be determined.
	/// </summary>
	uint32 GetVersion() const { return m_LocalVersion; }

	/// <summary>
	/// Loads up the directory and assembles the update.
	/// </summary>
	/// <param name="regenerateKeys">If true, the existing key pair is deleted and a new one is generated. Clients, which pinned the old public key, reject updates signed with the new one.</param>
	/// <returns>Returns true, if the update was built successfully and if the server is ready to run.</returns>
	bool LoadUpdateFile(bool regenerateKeys = false, bool skipDebugFiles = true);

	/// <summary>
	/// Starts to watch the folder with the binaries (hot reloading). Whenever its content changes, the update is rebuilt and offered to all clients, which ask afterwards.
	/// The folder can be changed in any way: files replaced, the whole folder deleted and created again, renamed or swapped. The changes have to be stable for a few
	/// seconds before the update is rebuilt, so copying files into the folder does not cause rebuilds in between. While the folder is missing, empty, or the
	/// rebuild fails, the previous update stays available.
	/// </summary>
	void StartFileWatcher();

private:

	bool Step();

	/// <summary>
	/// The version, which is announced: the configured one, otherwise the one in version.txt in the binary folder, otherwise the one from the source (CamVersion.h).
	/// </summary>
	uint32 ResolveVersion() const;

	/// <summary>
	/// Tells, if a file in the binary folder is part of the update.
	/// </summary>
	bool ShouldShip(const std::filesystem::directory_entry &entry, bool skipDebugFiles = true) const;

	/// <summary>
	/// Describes the shipped files of the binary folder (name, size, time), empty if the folder does not exist or has no files.
	/// </summary>
	std::string ComputeFingerprint() const;

	/// <summary>
	/// The thread, which detects changes in the binary folder.
	/// </summary>
	void WatchLoop();

private:

	ServerConfig m_Config;
	Core::Socket *m_Socket = nullptr;
	Core::Crypto *m_Crypto = nullptr;
	Core::IPTable *m_IPTable = nullptr;
	Core::Clients *m_Clients = nullptr;

	Core::Crypto::key_t m_PublicKey = {};

	// Guards the update data (file, signature, public key). The file watcher rebuilds the update on its own thread, while the network thread sends it.
	std::mutex m_UpdateMutex;

	// The hot reloading thread and the state of the folder, for which the current update was built.
	std::thread m_WatchThread;
	std::atomic<bool> m_WatchRunning{ false };
	std::string m_BuiltFingerprint;

	// Update data.
	int64 m_LastUpdateCheckMS;
	int64 m_LastUpdateWriteMS;
	uint32 m_LocalVersion = 0;
	Signature m_UpdateSignature;
	Core::FileSystemBuffer m_UpdateFile;
};

