#pragma once

#include "Core/Core.h"
#include "Core/Crypto.h"

#include "Socket.h"
#include "IPTable.h"

namespace Core
{
	class Clients
	{
	public:

		class Node
		{
		public:
			Node *Next;
			addr_t Addr;
			uint64 ServerToken;
			int64 TimeMS;
			uint32 Bandwidth;

			// Updates current bandwidth limits. Returns true if node has bandwidth available to it.
			bool IsBandwidthAvailable(int64 now_ms);

		protected:
			friend class Clients;

			// Rate to decrease bandwidth usage, bytes/ms (the speed limit per client, 1000 bytes/ms are 1 MB/s).
			static inline int64 s_DecreaseRate = 10000;

			// Maximum bandwidth to transfer at once, bytes: the burst, which is allowed before the speed limit applies.
			static inline uint32 s_MaxBandwidth = 2500000;

			// No speed limit at all.
			static inline bool s_Unlimited = false;
		};

		/// <summary>
		/// Sets the maximum speed, with which data is sent to a single client, in bytes per second. Applies to all clients. 0 means no limit.
		/// </summary>
		static void SetSpeedLimit(uint32 bytes_per_second)
		{
			Node::s_Unlimited = (bytes_per_second == 0);
			Node::s_DecreaseRate = bytes_per_second / 1000 > 0 ? bytes_per_second / 1000 : 1;

			// The burst is a quarter of a second, but at least 256 KB. Larger bursts could overflow the receive buffer of the client.
			Node::s_MaxBandwidth = bytes_per_second / 4 > 262144 ? bytes_per_second / 4 : 262144;
		}

		// Initializes the clients table.
		Clients();
		Clients(Crypto *crypto, IPTable *iptable);

		// Resets the clients table.
		void Reset();

		// Inserts or returns the client for the given address.
		Node *Insert(addr_t addr, int64 now_ms);

		// Removes the client associated with the given address.
		void Remove(addr_t addr);

	protected:

		Crypto *m_Crypto = nullptr;
		IPTable *m_IPTable = nullptr;
		uint32 m_Num;
		Node m_Data[65536];
		Node *m_Table[32768];
	};
}

