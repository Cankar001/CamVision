#include "Client.h"

#include <iostream>
#include <assert.h>
#include <miniz/miniz.h>

#include "Utils/Utils.h"

Client::Client(const ClientConfig &config)
	: m_Config(config)
{
	m_Socket = Core::Socket::Create();
	m_Crypto = Core::Crypto::Create();

	if (!m_Socket->Open(true, m_Config.ServerIP, m_Config.Port))
	{
		std::cerr << "Socket could not be opened!" << std::endl;
	}

	if (!m_Socket->SetNonBlocking(true))
	{
		std::cerr << "Socket could not be set to non-blocking!" << std::endl;
	}

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
	// First load the local client version
	uint32 local_version = Core::utils::GetLocalVersion(m_Config.UpdateTargetPath);
	if (!local_version)
	{
		std::cerr << "Could not retrieve the local version!" << std::endl;
		return;
	}

	std::cout << "Local version: " << local_version << std::endl;
	m_LocalVersion = local_version;

	// Then request the latest client version from the server
	std::cout << "Sending server version request..." << std::endl;
	ClientWantsVersionMessage message = {};
	message.LocalVersion = local_version;
	message.ClientVersion = 0;
	message.Header.Type = MessageType::CLIENT_REQUEST_VERSION;
	message.Header.Version = local_version;
	uint32 bytes_sent = m_Socket->Send(&message, sizeof(message), m_Host);
	assert(bytes_sent == sizeof(message));

	m_Status.Code = ClientStatusCode::NONE;
}

void Client::Run()
{
	// When running for the first time, request the server version
	RequestServerVersion();

	for (;;)
	{
		if (m_Status.Code == ClientStatusCode::UP_TO_DATE)
		{
			if (ExtractUpdate(m_Config.UpdateBinaryPath + "/update.zip"))
			{
				// TODO: start CamClient application
				std::cout << "Starting CamClient..." << std::endl;

				// Update local version
				m_LocalVersion = Core::utils::GetLocalVersion(m_Config.UpdateTargetPath);

				m_Status.Code = ClientStatusCode::NONE;
			}
			else
			{
				std::cerr << "Could not extract the archive!" << std::endl;
			}
		}
		else if (m_Status.Code == ClientStatusCode::BAD_SIG)
		{
			std::cerr << "ERROR: Bad Signature from last update." << std::endl;
		}
		else if (m_Status.Code == ClientStatusCode::BAD_WRITE)
		{
			std::cerr << "ERROR: Bad Write from last update." << std::endl;
		}

		MessageLoop();

		// Process updates.
		int64 now_ms = Core::QueryMS();
		UpdateProgress(now_ms, m_Host);

		static uint32 last_bytes;
		if (last_bytes != m_Status.Bytes)
		{
			last_bytes = m_Status.Bytes;
		}

		// Limit the update rate.
		Core::SleepMS(10);
	}
}

void Client::MessageLoop()
{
	for (;;)
	{
		static Byte BUF[65536];

		Core::addr_t addr;
		int32 len = m_Socket->Recv(BUF, sizeof(BUF), &addr);
		if (len < 0)
		{
			return;
		}

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
				return;
			}

			ServerVersionInfoMessage *msg = (ServerVersionInfoMessage *)BUF;
			std::cout << "Received new server version: " << msg->Version << std::endl;
			if (msg->Version != m_LocalVersion)
			{
				// The versions are different, we need an update
				m_Status.Code = ClientStatusCode::NEEDS_UPDATE;
				m_ClientVersion = msg->Version;

				// Copy the public key once
				m_Config.PublicKey.Size = msg->PublicKey.Size;
				memcpy(m_Config.PublicKey.Data, msg->PublicKey.Data, sizeof(msg->PublicKey.Data));
				
				// reset the update state, so that in the next update the update will start
				m_IsFinished = false;
				std::cout << "Requiring update..." << std::endl;
			}
			else
			{
				// The versions are the same, we have the latest version
				m_Status.Code = ClientStatusCode::UP_TO_DATE;
				std::cout << "binaries are up-to-date. No action required." << std::endl;
			}
		}
		else if (header->Type == MessageType::SERVER_UPDATE_BEGIN)
		{
			if (len != sizeof(ServerUpdateBeginMessage))
			{
				std::cerr << "received wrong package size" << std::endl;
				return;
			}

			ServerUpdateBeginMessage *msg = (ServerUpdateBeginMessage *)BUF;
			
			// Verify that the update size is reasonable (<200MB).
			if (msg->UpdateSize == 0 || msg->UpdateSize >= (200 * 1024 * 1024))
			{
				std::cerr << "Update size was very unrealistic! Size: " << msg->UpdateSize << std::endl;
				return;
			}

			// Allocate space for update data.
			if (!m_UpdateData.Alloc(msg->UpdateSize))
			{
				std::cerr << "Could not allocated enough space for update!" << std::endl;
				return;
			}

			// Allocate space for the piece tracker table.
			if (!m_UpdatePieces.Alloc((msg->UpdateSize + PIECE_BYTES - 1) / PIECE_BYTES))
			{
				std::cerr << "Could not allocated enough space for update!" << std::endl;
				return;
			}

			memcpy(&m_UpdateSignature, &msg->UpdateSignature, sizeof(Signature));
			std::cout << "Received update begin request, total size: " << msg->UpdateSize << std::endl;

			m_Status.Bytes = 0;
			m_Status.Total = m_UpdateData.Size;

			m_IsUpdating = true;
			m_IsFinished = false;
		}
		else if (header->Type == MessageType::SERVER_UPDATE_PIECE)
		{
			if (len != sizeof(ServerUpdatePieceMessage))
			{
				return;
			}

			if (!m_IsUpdating)
			{
				return;
			}

			ServerUpdatePieceMessage *msg = (ServerUpdatePieceMessage *)BUF;

			// Verify that the tokens match.
			if (msg->ClientToken != m_ClientToken || msg->ServerToken != m_ServerToken)
			{
				return;
			}

			// Verify that the message piece size is valid.
			if (msg->PieceSize > PIECE_BYTES)
			{
				return;
			}

			// Verify that the message contains the full piece data.
			if (len != (sizeof(ServerUpdatePieceMessage) + msg->PieceSize))
			{
				return;
			}

			// Verify that the position is on a piece boundry and something we actually requested.
			if ((msg->PiecePos % PIECE_BYTES) != 0)
			{
				return;
			}

			// Verify that the data doesn't write outside the buffer.
			if (msg->PiecePos + msg->PieceSize > m_UpdateData.Size)
			{
				return;
			}

			// Verify the piece position.
			uint32 idx = msg->PiecePos / PIECE_BYTES;
			if (idx >= m_UpdatePieces.Size)
			{
				return;
			}

			// Verify that we need the piece.
			if (m_UpdatePieces.Ptr[idx])
			{
				return;
			}

			// Validate that the piece size aligns with how we request data.
			if (idx < m_UpdatePieces.Size - 1)
			{
				if (msg->PieceSize != PIECE_BYTES)
				{
					return;
				}
			}

			memcpy(m_UpdateData.Ptr + msg->PiecePos, BUF + sizeof(ServerUpdatePieceMessage), msg->PieceSize);
			m_UpdatePieces.Ptr[idx] = 1;
			m_Status.Bytes += msg->PieceSize;
		}
		else if (header->Type == MessageType::SERVER_UPDATE_TOKEN)
		{
			if (len != sizeof(ServerUpdateTokenMessage))
			{
				return;
			}

			ServerUpdateTokenMessage *msg = (ServerUpdateTokenMessage *)BUF;
			if (msg->ClientToken != m_ClientToken)
			{
				return;
			}

			std::cout << "Received new server token: " << msg->ServerToken << std::endl;
			m_ServerToken = msg->ServerToken;
		}
	}
}

bool Client::ExtractUpdate(const std::string &zipPath)
{
	mz_zip_archive zip_archive;

	std::cout << "Extracting archive " << zipPath.c_str() << "..." << std::endl;

	mz_bool status = mz_zip_reader_init_file(&zip_archive, zipPath.c_str(), 0);
	if (!status)
	{
		std::cerr << "Could not open the ZIP file!" << std::endl;
		return false;
	}

	for (uint32 i = 0; i < mz_zip_reader_get_num_files(&zip_archive); ++i)
	{
		mz_zip_archive_file_stat file_stat;
		if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat))
		{
			std::cerr << "Could not make the file stats" << std::endl;

			if (!mz_zip_reader_end(&zip_archive))
			{
				std::cerr << "Could not close the zip reader" << std::endl;
			}

			return false;
		}

		std::string file_name = file_stat.m_filename;
		std::string file_name_on_disk = m_Config.UpdateBinaryPath + "/" + file_name;
		std::string comment = file_stat.m_comment;
		uint64 uncompressed_size = file_stat.m_uncomp_size;
		uint64 compressed_size = file_stat.m_comp_size;
		mz_bool is_dir = mz_zip_reader_is_file_a_directory(&zip_archive, i);
		std::cout << "Extracting file " << file_name.c_str() << std::endl;

		Byte *p = (Byte*)mz_zip_reader_extract_file_to_heap(&zip_archive, file_name.c_str(), &uncompressed_size, 0);

		if (!Core::FileSystem::Get()->WriteFile(file_name_on_disk, p, (uint32)uncompressed_size))
		{
			std::cerr << "Could not write the file " << file_name_on_disk.c_str() << std::endl;
			return false;
		}

		delete[] p;
		p = nullptr;
	}

	if (!mz_zip_reader_end(&zip_archive))
	{
		std::cerr << "Could not close the zip reader" << std::endl;
		return false;
	}

	std::cout << "Archive " << zipPath.c_str() << " extracted." << std::endl;
	return true;
}

void Client::Reset()
{
	m_UpdateData.Free();
	m_UpdatePieces.Free();

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
			std::cout << "Sending update begin request..." << std::endl;
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
	std::cout << "Update index: " << m_UpdateIdx << ", pieces size: " << m_UpdatePieces.Size << std::endl;
	if (m_UpdateIdx >= m_UpdatePieces.Size)
	{
	//	if (m_Crypto->TestSignature(m_UpdateSignature.Data, SIG_BYTES, m_UpdateData.Ptr, m_UpdateData.Size, m_Config.PublicKey.Data, m_Config.PublicKey.Size))
	//	{
			std::cout << "Writing file " << (m_Config.UpdateBinaryPath + "/update.zip") << std::endl;
			if (Core::FileSystem::Get()->WriteFile(m_Config.UpdateBinaryPath + "/update.zip", m_UpdateData.Ptr, m_UpdateData.Size))
			{
				m_IsFinished = true;
				m_Status.Code = ClientStatusCode::UP_TO_DATE;
				std::cout << "File " << (m_Config.UpdateBinaryPath + "/update.zip") << " written successfully." << std::endl;
				return;
			}
			else
			{
				m_Status.Code = ClientStatusCode::BAD_WRITE;
			}
	//	}
	//	else
	//	{
	//		m_Status.Code = ClientStatusCode::BAD_SIG;
	//	}

		Reset();
		return;
	}

	// Update in progress, request pieces.
	if (now_ms - m_LastPieceMS >= 100)
	{
		m_LastPieceMS = now_ms;

		std::cout << "Sending update piece request..." << std::endl;
		ClientUpdatePieceMessage msg = {};
		msg.Header.Version = m_LocalVersion;
		msg.Header.Type = MessageType::CLIENT_UPDATE_PIECE;
		msg.ClientToken = m_ClientToken;
		msg.ServerToken = m_ServerToken;

		bool found_missing = false;
		uint32 end_idx = 0;
		uint32 num_pieces = 0;

		// Request missing piecees.
		for (uint32 idx = m_UpdateIdx, end = m_UpdatePieces.Size; idx < end; ++idx)
		{
			if (!m_UpdatePieces.Ptr[idx])
			{
				// Keep track of the last valid idx.
				if (!found_missing)
				{
					found_missing = true;
					end_idx = idx;
				}

				msg.PiecePos = idx * PIECE_BYTES;
				m_Socket->Send(&msg, sizeof(msg), m_Host);

				num_pieces += 1;
				if (num_pieces > MAX_REQUESTS)
				{
					// Don't send too many.
					break;
				}
			}
		}

		if (end_idx > m_UpdateIdx)
		{
			m_UpdateIdx = end_idx;
		}

		if (!found_missing)
		{
			m_UpdateIdx = m_UpdatePieces.Size;
			std::cout << "Requested all update pieces successfully." << std::endl;
		}
	}
}

