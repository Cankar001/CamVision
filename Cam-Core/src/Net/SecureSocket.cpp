#include "SecureSocket.h"

#include "CamMessages.h"
#include "Core/Log.h"
#include "Core/Timer.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Core
{
	std::string AddressToString(addr_t addr)
	{
		// The address is stored in network order, the bytes are read one by one, so it works on every machine.
		const Byte *host = (const Byte *)&addr.Host;
		const Byte *port = (const Byte *)&addr.Port;
		return std::to_string(host[0]) + "." + std::to_string(host[1]) + "." + std::to_string(host[2]) + "." + std::to_string(host[3]) + ":" + std::to_string((port[0] << 8) | port[1]);
	}

	namespace
	{
		constexpr int64 PENDING_LIFETIME_MS = 10000;		// a hello, which was not followed by data, is forgotten
		constexpr int64 SESSION_IDLE_MS = 120000;			// a device, which was silent for this long, has to say hello again
		constexpr int64 HELLO_RESEND_MS = 500;
		constexpr int64 CONFIRM_TIMEOUT_MS = 3000;			// the device waits this long for the server to answer on the new connection, then it starts again
		constexpr int64 RESET_MIN_AGE_MS = 1000;			// a connection, which was made a moment ago, is not ended by a reset (answers to old messages)
		constexpr size_t MAX_PENDING_PER_ADDRESS = 4;
		constexpr size_t MAX_PENDING_TOTAL = 512;
		constexpr int64 SWEEP_INTERVAL_MS = 5000;
		constexpr int64 WARNING_INTERVAL_MS = 10000;
		constexpr size_t REPLAY_WINDOW = 1024;
		constexpr size_t MAX_MESSAGE_BYTES = 65535;

		void MakeNonce(uint64 counter, Byte nonce[crypto::NONCE_BYTES])
		{
			// 4 zero bytes and the counter. Every key is only used for one direction of one connection, so a counter never repeats with a key.
			memset(nonce, 0, crypto::NONCE_BYTES);
			for (int i = 0; i < 8; ++i)
			{
				nonce[4 + i] = (Byte)(counter >> (56 - 8 * i));
			}
		}

		// The keys of a connection: one for each direction, new for every connection, because the random numbers of both sides are part of them.
		void DeriveKeys(const Byte deviceKey[crypto::KEY_BYTES], const Byte clientRandom[AUTH_RANDOM_BYTES], const Byte serverRandom[AUTH_RANDOM_BYTES], Byte clientToServer[crypto::KEY_BYTES], Byte serverToClient[crypto::KEY_BYTES])
		{
			Byte salt[2 * AUTH_RANDOM_BYTES];
			memcpy(salt, clientRandom, AUTH_RANDOM_BYTES);
			memcpy(salt + AUTH_RANDOM_BYTES, serverRandom, AUTH_RANDOM_BYTES);

			static const char CLIENT_TO_SERVER[] = "CamVision connection v1 device to server";
			static const char SERVER_TO_CLIENT[] = "CamVision connection v1 server to device";
			crypto::HkdfSha256(salt, sizeof(salt), deviceKey, crypto::KEY_BYTES, CLIENT_TO_SERVER, (uint32)sizeof(CLIENT_TO_SERVER) - 1, clientToServer, crypto::KEY_BYTES);
			crypto::HkdfSha256(salt, sizeof(salt), deviceKey, crypto::KEY_BYTES, SERVER_TO_CLIENT, (uint32)sizeof(SERVER_TO_CLIENT) - 1, serverToClient, crypto::KEY_BYTES);
		}

		// Remembers which counters of a connection were seen, so no message is accepted twice. Messages may arrive out of order (UDP), so the last
		// REPLAY_WINDOW counters are tracked, older ones are refused.
		class ReplayWindow
		{
		public:

			bool Accept(uint64 counter)
			{
				if (!m_Any)
				{
					m_Any = true;
					m_Highest = counter;
					m_Seen.reset();
					m_Seen.set(0);
					return true;
				}

				if (counter > m_Highest)
				{
					uint64 shift = counter - m_Highest;
					if (shift >= REPLAY_WINDOW)
					{
						m_Seen.reset();
					}
					else
					{
						m_Seen <<= (size_t)shift;
					}

					m_Seen.set(0);
					m_Highest = counter;
					return true;
				}

				uint64 age = m_Highest - counter;
				if (age >= REPLAY_WINDOW || m_Seen.test((size_t)age))
				{
					return false;
				}

				m_Seen.set((size_t)age);
				return true;
			}

		private:

			bool m_Any = false;
			uint64 m_Highest = 0;
			std::bitset<REPLAY_WINDOW> m_Seen;
		};

		// One connection (one direction of keys each).
		struct Session
		{
			Byte SendKey[crypto::KEY_BYTES] = {};
			Byte ReceiveKey[crypto::KEY_BYTES] = {};
			uint64 SendCounter = 0;
			ReplayWindow Window;
			int64 CreatedMS = 0;
			int64 LastReceiveMS = 0;
			Device Info;				// server side: the device behind the connection
		};

		// A warning is not repeated every time, a flood of bad messages would flood the log.
		struct WarningLimit
		{
			int64 LastMS = 0;
			uint64 Suppressed = 0;

			bool Allow(int64 now, uint64 *suppressed)
			{
				if (LastMS != 0 && now - LastMS < WARNING_INTERVAL_MS)
				{
					++Suppressed;
					return false;
				}

				LastMS = now;
				*suppressed = Suppressed;
				Suppressed = 0;
				return true;
			}
		};
	}

	struct SecureSocket::Impl
	{
		std::unique_ptr<Socket> Inner;
		bool IsServer = false;

		mutable std::mutex Mutex;
		std::vector<Byte> ReceiveBuffer = std::vector<Byte>(MAX_MESSAGE_BYTES + 1);

		std::atomic<uint64> Rejected{ 0 };
		WarningLimit RejectedWarning;
		WarningLimit UnknownKeyWarning;
		WarningLimit PlainWarning;

		// ---- server
		DeviceRegistry *Registry = nullptr;
		std::map<uint64, Session> Active;							// by address
		std::map<uint64, std::vector<Session>> Pending;				// by address, hellos without data yet
		std::map<uint64, int64> LastReset;							// by address
		size_t PendingTotal = 0;
		int64 LastSweepMS = 0;

		// ---- device
		Byte DeviceKey[crypto::KEY_BYTES] = {};
		uint64 DeviceKeyId = 0;
		enum class State { Idle, HelloSent, Keyed } ConnectionState = State::Idle;
		Byte ClientRandom[AUTH_RANDOM_BYTES] = {};
		addr_t ServerAddress = {};
		int64 HelloMS = 0;
		int64 KeyedMS = 0;
		bool Confirmed = false;
		Session Connection;

		// ------------------------------------------------------------------------------------------------------------------------------
		int32 Seal(const Session &session, uint64 counter, const void *src, int32 bytes, std::vector<Byte> &out)
		{
			out.resize(sizeof(SecureDataHeader) + (size_t)bytes + crypto::TAG_BYTES);

			SecureDataHeader header = {};
			header.Header.Version = CAM_PROTOCOL_VERSION;
			header.Header.Type = MessageType::SECURE_DATA;
			header.Counter = counter;
			memcpy(out.data(), &header, sizeof(header));

			Byte nonce[crypto::NONCE_BYTES];
			MakeNonce(counter, nonce);

			Byte *cipher = out.data() + sizeof(header);
			Byte *tag = cipher + bytes;
			if (!crypto::AesGcmSeal(session.SendKey, nonce, out.data(), (uint32)sizeof(header), src, (uint32)bytes, cipher, tag))
			{
				return -1;
			}

			return (int32)out.size();
		}

		// Opens a message with the key of a connection (without checking the counter). Returns the size of the message, or -1.
		int32 Open(const Byte key[crypto::KEY_BYTES], const Byte *data, int32 length, void *dst, int32 dstBytes, uint64 *counter)
		{
			if (length < (int32)(sizeof(SecureDataHeader) + crypto::TAG_BYTES))
			{
				return -1;
			}

			SecureDataHeader header;
			memcpy(&header, data, sizeof(header));

			int32 bytes = length - (int32)sizeof(header) - (int32)crypto::TAG_BYTES;
			if (bytes > dstBytes)
			{
				return -1;
			}

			Byte nonce[crypto::NONCE_BYTES];
			MakeNonce(header.Counter, nonce);

			const Byte *cipher = data + sizeof(header);
			const Byte *tag = cipher + bytes;
			if (!crypto::AesGcmOpen(key, nonce, data, (uint32)sizeof(header), cipher, (uint32)bytes, tag, dst))
			{
				return -1;
			}

			*counter = header.Counter;
			return bytes;
		}

		void Reject(int64 now, const char *what, addr_t from)
		{
			++Rejected;
			uint64 suppressed = 0;
			if (RejectedWarning.Allow(now, &suppressed))
			{
				CAM_LOG_WARN("Dropped a message from {0}: {1}.{2}", AddressToString(from), what, suppressed ? " (" + std::to_string(suppressed) + " more like it since the last warning)" : "");
			}
		}

		void SendReset(addr_t to, int64 now)
		{
			// At most once per second per address, and not for an unlimited number of addresses.
			auto last = LastReset.find(to.Value);
			if (last != LastReset.end() && now - last->second < 1000)
			{
				return;
			}

			if (LastReset.size() > 1024)
			{
				LastReset.clear();
			}

			LastReset[to.Value] = now;

			AuthResetMessage reset = {};
			reset.Header.Version = CAM_PROTOCOL_VERSION;
			reset.Header.Type = MessageType::AUTH_RESET;
			Inner->Send(&reset, sizeof(reset), to);
		}

		// ------------------------------------------------------------------------------------------------------------------------------ server
		void Sweep(int64 now, bool force = false)
		{
			if (!force && now - LastSweepMS < SWEEP_INTERVAL_MS)
			{
				return;
			}

			LastSweepMS = now;

			// Devices can be added and removed in the file, while the server runs.
			if (Registry && Registry->ReloadIfChanged())
			{
				CAM_LOG_INFO("The list of devices ({0}) was read again, {1} devices.", Registry->File(), Registry->Count());
			}

			// A device, which was removed (or got a new key), does not stay connected.
			for (auto it = Active.begin(); Registry && it != Active.end();)
			{
				Device current;
				if (!Registry->Find(it->second.Info.KeyId, &current))
				{
					CAM_LOG_WARN("The device '{0}' is not in the list anymore, ending its connection.", it->second.Info.Name);
					it = Active.erase(it);
				}
				else
				{
					++it;
				}
			}

			for (auto it = Active.begin(); it != Active.end();)
			{
				if (now - it->second.LastReceiveMS > SESSION_IDLE_MS)
				{
					it = Active.erase(it);
				}
				else
				{
					++it;
				}
			}

			PendingTotal = 0;
			for (auto it = Pending.begin(); it != Pending.end();)
			{
				auto &list = it->second;
				list.erase(std::remove_if(list.begin(), list.end(), [&](const Session &s) { return now - s.CreatedMS > PENDING_LIFETIME_MS; }), list.end());
				if (list.empty())
				{
					it = Pending.erase(it);
				}
				else
				{
					PendingTotal += list.size();
					++it;
				}
			}
		}

		void ServerHello(addr_t from, const Byte *data, int32 length, int64 now)
		{
			if (length != (int32)sizeof(AuthHelloMessage))
			{
				return;
			}

			AuthHelloMessage hello;
			memcpy(&hello, data, sizeof(hello));

			Device device;
			if (!Registry || !Registry->Find(hello.KeyId, &device))
			{
				// Nothing is answered: somebody without a valid key gets nothing.
				++Rejected;
				uint64 suppressed = 0;
				if (UnknownKeyWarning.Allow(now, &suppressed))
				{
					CAM_LOG_WARN("{0} tried to connect with a device key, which is not in the list of devices.{1}", AddressToString(from), suppressed ? " (" + std::to_string(suppressed) + " more tries since the last warning)" : "");
				}

				return;
			}

			if (PendingTotal >= MAX_PENDING_TOTAL)
			{
				Sweep(now, true);
				if (PendingTotal >= MAX_PENDING_TOTAL)
				{
					return;
				}
			}

			Session session;
			session.Info = device;
			session.CreatedMS = now;
			session.LastReceiveMS = now;

			AuthChallengeMessage challenge = {};
			challenge.Header.Version = CAM_PROTOCOL_VERSION;
			challenge.Header.Type = MessageType::AUTH_CHALLENGE;
			memcpy(challenge.ClientRandom, hello.ClientRandom, AUTH_RANDOM_BYTES);
			if (!crypto::RandomBytes(challenge.ServerRandom, AUTH_RANDOM_BYTES))
			{
				return;
			}

			// The server receives what the device sends (device to server), and sends the other direction.
			DeriveKeys(device.Key, hello.ClientRandom, challenge.ServerRandom, session.ReceiveKey, session.SendKey);

			// The connection, which exists already, stays until the device proves with a message, that it knows the key (a hello alone can be sent by anybody
			// in the name of another address, it must not end the connection of the real device).
			std::vector<Session> &list = Pending[from.Value];
			if (list.size() >= MAX_PENDING_PER_ADDRESS)
			{
				list.erase(list.begin());
				--PendingTotal;
			}

			list.push_back(session);
			++PendingTotal;

			Inner->Send(&challenge, sizeof(challenge), from);
		}

		int32 ServerData(addr_t from, const Byte *data, int32 length, void *dst, int32 dstBytes, int64 now)
		{
			uint64 counter = 0;

			auto active = Active.find(from.Value);
			if (active != Active.end())
			{
				int32 bytes = Open(active->second.ReceiveKey, data, length, dst, dstBytes, &counter);
				if (bytes >= 0)
				{
					if (!active->second.Window.Accept(counter))
					{
						++Rejected;		// a message, which was received before: a replay (or a duplicate of the network)
						return 0;
					}

					active->second.LastReceiveMS = now;
					return bytes;
				}
			}

			auto pending = Pending.find(from.Value);
			if (pending != Pending.end())
			{
				for (Session &candidate : pending->second)
				{
					int32 bytes = Open(candidate.ReceiveKey, data, length, dst, dstBytes, &counter);
					if (bytes < 0)
					{
						continue;
					}

					// The device knows its key: the connection is real now, and replaces the old one of this address.
					Session session = candidate;
					session.Window.Accept(counter);
					session.LastReceiveMS = now;

					PendingTotal -= std::min(PendingTotal, pending->second.size());
					Pending.erase(pending);
					Active[from.Value] = session;

					CAM_LOG_INFO("The {0} '{1}' connected securely from {2}.", DeviceRoleName(session.Info.Role), session.Info.Name, AddressToString(from));
					return bytes;
				}
			}

			if (active == Active.end() && pending == Pending.end())
			{
				// Not known at all (the server was restarted, or the connection timed out): tell the device, to start again.
				SendReset(from, now);
				return 0;
			}

			Reject(now, "it could not be opened (wrong key, changed, or too large)", from);
			return 0;
		}

		// ------------------------------------------------------------------------------------------------------------------------------ device
		void SendHello(addr_t to, int64 now)
		{
			AuthHelloMessage hello = {};
			hello.Header.Version = CAM_PROTOCOL_VERSION;
			hello.Header.Type = MessageType::AUTH_HELLO;
			hello.KeyId = DeviceKeyId;
			memcpy(hello.ClientRandom, ClientRandom, AUTH_RANDOM_BYTES);
			Inner->Send(&hello, sizeof(hello), to);
			HelloMS = now;
		}

		void DeviceChallenge(addr_t from, const Byte *data, int32 length, int64 now)
		{
			if (length != (int32)sizeof(AuthChallengeMessage) || ConnectionState != State::HelloSent)
			{
				return;
			}

			AuthChallengeMessage challenge;
			memcpy(&challenge, data, sizeof(challenge));
			if (memcmp(challenge.ClientRandom, ClientRandom, AUTH_RANDOM_BYTES) != 0)
			{
				return;
			}

			Connection = Session();
			DeriveKeys(DeviceKey, ClientRandom, challenge.ServerRandom, Connection.SendKey, Connection.ReceiveKey);
			Connection.CreatedMS = now;
			Connection.LastReceiveMS = now;
			ConnectionState = State::Keyed;
			KeyedMS = now;
			Confirmed = false;
			(void)from;
		}

		int32 DeviceData(addr_t from, const Byte *data, int32 length, void *dst, int32 dstBytes, int64 now)
		{
			if (ConnectionState != State::Keyed)
			{
				return 0;
			}

			uint64 counter = 0;
			int32 bytes = Open(Connection.ReceiveKey, data, length, dst, dstBytes, &counter);
			if (bytes < 0)
			{
				Reject(now, "it could not be opened (not from the server, or changed)", from);
				return 0;
			}

			if (!Connection.Window.Accept(counter))
			{
				++Rejected;
				return 0;
			}

			Connection.LastReceiveMS = now;
			if (!Confirmed)
			{
				Confirmed = true;
				CAM_LOG_INFO("Secure connection to the server {0} established.", AddressToString(from));
			}

			return bytes;
		}

		int32 ReceiveOne(addr_t from, int32 length, void *dst, int32 dstBytes)
		{
			std::lock_guard<std::mutex> lock(Mutex);
			int64 now = Core::QueryMS();

			if (length < (int32)sizeof(header_t))
			{
				return 0;
			}

			const Byte *data = ReceiveBuffer.data();
			header_t header;
			memcpy(&header, data, sizeof(header));

			if (IsServer)
			{
				Sweep(now);

				switch (header.Type)
				{
					case MessageType::AUTH_HELLO: ServerHello(from, data, length, now); return 0;
					case MessageType::SECURE_DATA: return ServerData(from, data, length, dst, dstBytes, now);
					default: break;
				}

				// A message, which is not secure. The server does not talk to somebody without a key.
				++Rejected;
				uint64 suppressed = 0;
				if (PlainWarning.Allow(now, &suppressed))
				{
					CAM_LOG_WARN("{0} sent a message without a device key. The server only talks to devices with a key, see 'CamServer --add_device'.{1}", AddressToString(from), suppressed ? " (" + std::to_string(suppressed) + " more since the last warning)" : "");
				}

				return 0;
			}

			if (from.Value != ServerAddress.Value)
			{
				return 0;
			}

			switch (header.Type)
			{
				case MessageType::AUTH_CHALLENGE:
					DeviceChallenge(from, data, length, now);
					return 0;

				case MessageType::SECURE_DATA:
					return DeviceData(from, data, length, dst, dstBytes, now);

				case MessageType::AUTH_RESET:
					// The server does not know this connection (anymore): start again, with the next message to send.
					if (ConnectionState == State::Keyed && now - KeyedMS > RESET_MIN_AGE_MS)
					{
						ConnectionState = State::Idle;
					}

					return 0;

				default:
					break;
			}

			// Not secure, from the server: it does not use device keys, or it is not the server.
			++Rejected;
			uint64 suppressed = 0;
			if (PlainWarning.Allow(now, &suppressed))
			{
				CAM_LOG_WARN("The server {0} sent a message, which is not secure. Does it run without device keys (auth = false in server.cfg)? Then remove the key from this device.", AddressToString(from));
			}

			return 0;
		}
	};

	SecureSocket::SecureSocket(Impl *impl)
		: m_Impl(impl)
	{
	}

	SecureSocket::~SecureSocket()
	{
		delete m_Impl;
		m_Impl = nullptr;
	}

	SecureSocket *SecureSocket::CreateClient(Socket *inner, const Byte key[crypto::KEY_BYTES])
	{
		Impl *impl = new Impl();
		impl->Inner.reset(inner);
		impl->IsServer = false;
		memcpy(impl->DeviceKey, key, crypto::KEY_BYTES);
		impl->DeviceKeyId = Core::DeviceKeyId(key);
		return new SecureSocket(impl);
	}

	SecureSocket *SecureSocket::CreateServer(Socket *inner, DeviceRegistry *registry)
	{
		Impl *impl = new Impl();
		impl->Inner.reset(inner);
		impl->IsServer = true;
		impl->Registry = registry;
		return new SecureSocket(impl);
	}

	bool SecureSocket::Open(bool is_client, const std::string &ip, uint16 port) { return m_Impl->Inner->Open(is_client, ip, port); }
	void SecureSocket::Close() { m_Impl->Inner->Close(); }
	bool SecureSocket::Bind(uint16 port) { return m_Impl->Inner->Bind(port); }
	bool SecureSocket::BindLoopback(uint16 port) { return m_Impl->Inner->BindLoopback(port); }
	bool SecureSocket::SetNonBlocking(bool enabled) { return m_Impl->Inner->SetNonBlocking(enabled); }
	bool SecureSocket::SetBufferSizes(int32 send_bytes, int32 recv_bytes) { return m_Impl->Inner->SetBufferSizes(send_bytes, recv_bytes); }
	addr_t SecureSocket::Lookup(const std::string &host, uint16 port) { return m_Impl->Inner->Lookup(host, port); }

	int32 SecureSocket::Recv(void *dst, int32 dst_bytes, addr_t *addr)
	{
		// Messages of the handshake and the ones, which are refused, are not given to the caller: go on with the next one.
		for (int round = 0; round < 256; ++round)
		{
			addr_t from = {};
			int32 length = m_Impl->Inner->Recv(m_Impl->ReceiveBuffer.data(), (int32)m_Impl->ReceiveBuffer.size(), &from);
			if (length <= 0)
			{
				return length;
			}

			int32 bytes = m_Impl->ReceiveOne(from, length, dst, dst_bytes);
			if (bytes > 0)
			{
				*addr = from;
				return bytes;
			}
		}

		return 0;
	}

	int32 SecureSocket::Send(void const *src, int32 src_bytes, addr_t addr)
	{
		if (src_bytes < 0 || (size_t)src_bytes + OVERHEAD > MAX_MESSAGE_BYTES)
		{
			return -1;
		}

		Session session;
		uint64 counter = 0;
		{
			std::lock_guard<std::mutex> lock(m_Impl->Mutex);
			int64 now = Core::QueryMS();

			if (m_Impl->IsServer)
			{
				auto found = m_Impl->Active.find(addr.Value);
				if (found == m_Impl->Active.end())
				{
					// No connection with this address (anymore): nothing is sent, like a lost message.
					return src_bytes;
				}

				counter = found->second.SendCounter++;
				memcpy(session.SendKey, found->second.SendKey, crypto::KEY_BYTES);
			}
			else
			{
				Impl &device = *m_Impl;
				device.ServerAddress = addr;

				if (device.ConnectionState == Impl::State::Keyed && !device.Confirmed && now - device.KeyedMS > CONFIRM_TIMEOUT_MS)
				{
					// The server did not answer on the new connection (it does not know the key, or the messages got lost): start again.
					device.ConnectionState = Impl::State::Idle;
				}

				if (device.ConnectionState == Impl::State::Idle)
				{
					if (!crypto::RandomBytes(device.ClientRandom, AUTH_RANDOM_BYTES))
					{
						return -1;
					}

					device.ConnectionState = Impl::State::HelloSent;
					device.SendHello(addr, now);
					return src_bytes;
				}

				if (device.ConnectionState == Impl::State::HelloSent)
				{
					// Until the server answered, the messages wait in the sense, that they are sent again by the program (it repeats its requests).
					if (now - device.HelloMS >= HELLO_RESEND_MS)
					{
						device.SendHello(addr, now);
					}

					return src_bytes;
				}

				counter = device.Connection.SendCounter++;
				memcpy(session.SendKey, device.Connection.SendKey, crypto::KEY_BYTES);
			}
		}

		// Sealing and sending need no lock: the key is a copy, and the counter is unique.
		thread_local std::vector<Byte> packet;
		int32 packet_bytes = m_Impl->Seal(session, counter, src, src_bytes, packet);
		if (packet_bytes < 0)
		{
			return -1;
		}

		int32 sent = m_Impl->Inner->Send(packet.data(), packet_bytes, addr);
		return sent == packet_bytes ? src_bytes : -1;
	}

	bool SecureSocket::GetPeer(addr_t addr, Peer *out) const
	{
		std::lock_guard<std::mutex> lock(m_Impl->Mutex);
		auto found = m_Impl->Active.find(addr.Value);
		if (found == m_Impl->Active.end())
		{
			return false;
		}

		out->Role = found->second.Info.Role;
		out->Name = found->second.Info.Name;
		out->KeyId = found->second.Info.KeyId;
		return true;
	}

	void SecureSocket::UpdateDevices()
	{
		std::lock_guard<std::mutex> lock(m_Impl->Mutex);
		m_Impl->Sweep(Core::QueryMS(), true);
	}

	void SecureSocket::DropPeer(addr_t addr)
	{
		std::lock_guard<std::mutex> lock(m_Impl->Mutex);
		m_Impl->Active.erase(addr.Value);
	}

	uint32 SecureSocket::PeerCount() const
	{
		std::lock_guard<std::mutex> lock(m_Impl->Mutex);
		return (uint32)m_Impl->Active.size();
	}

	bool SecureSocket::IsEstablished() const
	{
		std::lock_guard<std::mutex> lock(m_Impl->Mutex);
		return !m_Impl->IsServer && m_Impl->ConnectionState == Impl::State::Keyed && m_Impl->Confirmed;
	}

	uint64 SecureSocket::RejectedCount() const
	{
		return m_Impl->Rejected;
	}
}
