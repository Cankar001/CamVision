#pragma once

#include "Core/Core.h"
#include "Core/CryptoPrimitives.h"

#include "DeviceRegistry.h"
#include "Socket.h"

#include <string>

namespace Core
{
	/// <summary>
	/// "192.168.1.20:51234" (for logs).
	/// </summary>
	std::string AddressToString(addr_t addr);

	/// <summary>
	/// A socket, which authenticates and encrypts everything it sends and receives. It wraps another socket and has the same interface, so the programs
	/// use it like the normal one.
	///
	/// How it works (every device has its own secret key, which is known to the device and to the server only):
	///  1. The device says "hello" with the id of its key (not the key) and a random number. The server answers with a random number of its own.
	///  2. Both make the keys of this connection (one per direction) out of the device key and the two random numbers. They are new for every connection,
	///     so recorded messages of an earlier connection are useless, and the device key itself is never used for data.
	///  3. Every message is sealed with AES-256-GCM: nobody can read it, and nobody can change it or make one up without the key. A counter in every
	///     message is its nonce and tells replayed messages (the server only forgets the counters, it never accepts a message twice).
	///  Only the real server knows the key, so a message which opens proves that the other side is who it says (this works in both directions).
	///  A message, which does not open, is dropped without an answer, so somebody without a key gets nothing back to work with.
	///
	/// The server side knows the devices (DeviceRegistry), and tells which device is behind an address (GetPeer): its name and what it is allowed to do.
	/// The programs, which use it, only have to check that: a display must not send frames, a camera must not ask for them.
	///
	/// Receiving must only be done by one thread, sending can be done from several threads.
	/// </summary>
	class SecureSocket : public Socket
	{
	public:

		/// <summary>
		/// What the bytes of a message grow by (counter header and tag).
		/// </summary>
		static constexpr uint32 OVERHEAD = 12 + 16;

		struct Peer
		{
			DeviceRole Role = DeviceRole::None;
			std::string Name;
			uint64 KeyId = 0;
		};

		/// <summary>
		/// The device side (a camera or a display): talks to one server with its key. Takes the ownership of the socket.
		/// </summary>
		static SecureSocket *CreateClient(Socket *inner, const Byte key[crypto::KEY_BYTES]);

		/// <summary>
		/// The server side: talks to all devices of the registry (not owned, it must live longer than the socket). Takes the ownership of the socket.
		/// </summary>
		static SecureSocket *CreateServer(Socket *inner, DeviceRegistry *registry);

		~SecureSocket() override;

		// The interface of the socket: Recv returns the opened messages, Send seals them. Messages, which are not secure, never come out of Recv.
		bool Open(bool is_client = false, const std::string &ip = "", uint16 port = 0) override;
		void Close() override;
		bool Bind(uint16 port) override;
		bool BindLoopback(uint16 port) override;
		int32 Recv(void *dst, int32 dst_bytes, addr_t *addr) override;
		int32 Send(void const *src, int32 src_bytes, addr_t addr) override;
		bool SetNonBlocking(bool enabled) override;
		bool SetBufferSizes(int32 send_bytes, int32 recv_bytes) override;
		addr_t Lookup(const std::string &host, uint16 port) override;

		/// <summary>
		/// Server: the device behind an address (it has a secure connection). Returns false, if there is none.
		/// </summary>
		bool GetPeer(addr_t addr, Peer *out) const;

		/// <summary>
		/// Server: reads the list of devices again, if the file changed, and ends the connections of devices, which are not in it anymore. This also happens
		/// on its own every few seconds, while messages arrive.
		/// </summary>
		void UpdateDevices();

		/// <summary>
		/// Server: ends the connection of an address. The device has to start again with the hello.
		/// </summary>
		void DropPeer(addr_t addr);

		/// <summary>
		/// Server: the number of devices with a secure connection.
		/// </summary>
		uint32 PeerCount() const;

		/// <summary>
		/// Device: true, if the server was heard from on the secure connection (so it is the real one, it knows the key).
		/// </summary>
		bool IsEstablished() const;

		/// <summary>
		/// The number of messages, which were dropped, because they were not secure, or did not open (wrong key, damaged, replayed).
		/// </summary>
		uint64 RejectedCount() const;

	private:

		struct Impl;
		SecureSocket(Impl *impl);

		Impl *m_Impl = nullptr;
	};
}
