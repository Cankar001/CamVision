#include "ServerClients.h"

#include "Core/Hash.h"
#include "Utils/Utils.h"

#ifdef CAM_PLATFORM_LINUX
#include <cstring>
#endif

namespace Core
{
	bool Clients::Node::IsBandwidthAvailable(int64 now_ms)
	{
		int64 decrease = (now_ms - TimeMS);
		TimeMS = now_ms;

		if (decrease > 0)
		{
			decrease *= s_DecreaseRate; // bytes/ms

			if ((int64)Bandwidth > decrease)
			{
				Bandwidth -= (uint32)decrease;
			}
			else
			{
				Bandwidth = 0;
			}
		}

		return s_Unlimited || (Bandwidth < s_MaxBandwidth);
	}

	Clients::Clients()
	{
		m_Crypto = nullptr;
		m_IPTable = nullptr;
		m_Num = 0;
		Reset();
	}

	Clients::Clients(Crypto *crypto, IPTable *iptable)
		: m_Crypto(crypto), m_IPTable(iptable), m_Num(0)
	{
		Reset();
	}
	
	void Clients::Reset()
	{
		m_Num = 0;
		memset(m_Table, 0, sizeof(m_Table));
	}
	
	Clients::Node *Clients::Insert(addr_t addr, int64 now_ms)
	{
		uint64 slot = Core::Mix64(addr.Value) % utils::Count(m_Table);
		Node *node = m_Table[slot];

		while (node)
		{
			if (node->Addr.Value == addr.Value)
			{
				break;
			}

			node = node->Next;
		}

		if (!node)
		{
			if (m_IPTable->Blocked(addr.Host))
			{
				return nullptr;
			}

			if (m_Num < utils::Count(m_Data))
			{
				node = &m_Data[m_Num++];
			}
			else
			{
				for (Node &iter : m_Data)
				{
					if (now_ms - iter.TimeMS > 30000)
					{
						node = &iter;
						break;
					}
				}

				if (!node)
				{
					return nullptr;
				}

				Remove(addr);
			}

			memset(node, 0, sizeof(*node));
			node->Addr = addr;
			node->ServerToken = m_Crypto->GenToken();
			node->TimeMS = now_ms;
			node->Next = m_Table[slot];
			m_Table[slot] = node;

			m_IPTable->Insert(addr.Host, now_ms);
		}

		return node;
	}
	
	void Clients::Remove(addr_t addr)
	{
		auto slot = Core::Mix64(addr.Value) % utils::Count(m_Table);
		Node *node = m_Table[slot];
		Node *prev = nullptr;

		while (node)
		{
			if (node->Addr.Value == addr.Value)
			{
				if (prev)
				{
					prev->Next = node->Next;
				}
				else
				{
					m_Table[slot] = node->Next;
				}

				return;
			}

			prev = node;
			node = node->Next;
		}
	}
}

