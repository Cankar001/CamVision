#include "WindowsCrypto.h"

#ifdef CAM_PLATFORM_WINDOWS

#include <iostream>
#include <assert.h>

namespace Core
{
	WindowsCrypto::WindowsCrypto()
	{
		if (!CryptAcquireContextW(&m_HProv, 0, MS_ENH_RSA_AES_PROV, PROV_RSA_AES, 0))
		{
			m_HProv = 0;
		}
	}

	WindowsCrypto::~WindowsCrypto()
	{
		if (m_HProv)
		{
			CryptReleaseContext(m_HProv, 0);
			m_HProv = 0;
		}
	}
	
	bool WindowsCrypto::GenKeys(key_t *pub, key_t *pri)
	{
		assert(pub);
		assert(pri);

		if (m_HProv == 0)
		{
			std::cerr << "crypto not initialized" << std::endl;
			return false;
		}

		HCRYPTKEY key;
		if (!CryptGenKey(m_HProv, AT_KEYEXCHANGE, (4096 << 16) | CRYPT_ARCHIVABLE | CRYPT_EXPORTABLE, &key))
		{
			std::cerr << "CryptGenKey() failed." << std::endl;
			return false;
		}

		BOOL result = TRUE;

		// Clear the buffers, so nothing but the key is in them (they are stored and sent over the network as they are).
		memset(pub->Data, 0, sizeof(pub->Data));
		memset(pri->Data, 0, sizeof(pri->Data));

		DWORD public_size = (DWORD)sizeof(pub->Data);
		if (!CryptExportKey(key, 0, PUBLICKEYBLOB, 0, pub->Data, &public_size))
		{
			std::cerr << "Could not export public key! Error code: " << GetLastError() << std::endl;
			return FALSE;
		}

		pub->Size = public_size;

		DWORD private_size = (DWORD)sizeof(pri->Data);
		if (!CryptExportKey(key, 0, PRIVATEKEYBLOB, 0, pri->Data, &private_size))
		{
			std::cerr << "Could not export private key! Error code: " << GetLastError() << std::endl;
			return FALSE;
		}

		pri->Size = private_size;

		if (!CryptDestroyKey(key))
		{
			std::cerr << "Could not destroy the key!" << std::endl;
			return FALSE;
		}

		return result;
	}
	
	uint64 WindowsCrypto::GenToken()
	{
		uint64 result = 0;

		if (m_HProv)
		{
			CryptGenRandom(m_HProv, sizeof(result), (Byte*)&result);
		}

		return result;
	}
	
	bool WindowsCrypto::SignSignature(Byte *sig, uint32 sig_bytes, Byte const *data, uint32 data_size, Byte const *pri_key, uint32 pri_key_bytes)
	{
		if (m_HProv == 0)
		{
			return false;
		}

		bool result = false;

		if (HCRYPTKEY hkey; CryptImportKey(m_HProv, pri_key, pri_key_bytes, 0, 0, &hkey))
		{
			if (HCRYPTHASH hash; CryptCreateHash(m_HProv, CALG_SHA_256, 0, 0, &hash))
			{
				if (CryptHashData(hash, (Byte*)data, data_size, 0))
				{
					DWORD length = sig_bytes;
					result = CryptSignHashW(hash, AT_KEYEXCHANGE, 0, 0, sig, &length);
				}

				CryptDestroyHash(hash);
			}

			CryptDestroyKey(hkey);
		}

		return result;
	}
	
	bool WindowsCrypto::TestSignature(void const *sig, uint32 sig_bytes, void const *src, uint32 src_bytes, Byte const *pub_key, uint32 pub_key_bytes)
	{
		if (m_HProv == 0)
		{
			return false;
		}

		bool result = false;

		if (HCRYPTKEY hkey; CryptImportKey(m_HProv, pub_key, pub_key_bytes, 0, 0, &hkey))
		{
			if (HCRYPTHASH hash; CryptCreateHash(m_HProv, CALG_SHA_256, 0, 0, &hash))
			{
				if (CryptHashData(hash, (Byte const *)src, src_bytes, 0))
				{
					result = CryptVerifySignatureW(hash, (Byte const *)sig, sig_bytes, hkey, 0, 0);
				}

				CryptDestroyHash(hash);
			}

			CryptDestroyKey(hkey);
		}

		return result;
	}
}

#endif // CAM_PLATFORM_WINDOWS

