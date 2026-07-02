#define _WINSOCK_DEPRECATED_NO_WARNINGS


#define _PCH_STATIC_CONST

#include <xstring>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <InitGuid.h>
#include "IBonDriver2.h"
#include "binzume\http.h"
#include "./nlohmann/json.hpp"
#include "mmt4kConverter.h"

using namespace std;
using namespace Net;
using json = nlohmann::json;


#if !defined(_BONTUNER_H_)
#define _BONTUNER_H_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#define dllimport dllexport

#define TUNER_NAME "BonDriver_Mirakurun"
#define TUNER_NAME_W L"BonDriver_Mirakurun"

// 受信サイズ
#define TSDATASIZE	48128	// TSデータのサイズ 188 * 256

static char g_IniFilePath[MAX_PATH] = { '\0' };

// チューナ空間
typedef struct
{
	std::u8string name;
	size_t channel_base;
	size_t channel_num;
} TSpaceType;

typedef std::vector<TSpaceType> TSpaceTypes;
typedef TSpaceTypes::iterator  TSpaceTypesI;
TSpaceTypes g_SpaceTypes;

#define MAX_HOST_LEN	256
#define MAX_PORT_LEN	8
static char g_ServerHost[MAX_HOST_LEN];
static char g_ServerPort[MAX_PORT_LEN];
static int g_DecodeB25;
static int g_Priority;
static int g_Service_Split;
json g_Channel_JSON;
static int g_MagicPacket_Enable;
static char g_MagicPacket_TargetMAC[18];
static char g_MagicPacket_TargetIP[16];
#define MAGICPACKET_WAIT_SECONDS 20

// MMT/TLV(4K/8K)チャンネル判定用のtype名リスト(既定:BS4K)。一致するチャンネルは
// decodeパラメータを常に0にし(サーバー側はMMT/TLVを復号できないため)、
// GetTsStream内でMmt4kConverterを通してMPEG2-TSに変換してから返す。
static std::vector<std::string> g_MmtTypes;
#ifdef ENABLE_MMT4K
static char g_Mmt4kSmartCardReaderName[256];
static char g_Mmt4kCasProxyServer[256];
static char g_Mmt4kCustomWinscardDLL[MAX_PATH];
#endif

class CBonTuner : public IBonDriver2
{
public:
	CBonTuner();
	virtual ~CBonTuner();

	// Initialize channel
	void InitChannel(void);

	// IBonDriver
	const BOOL OpenTuner(void) override;
	void CloseTuner(void) override;

	const BOOL SetChannel(const BYTE bCh) override;
	const float GetSignalLevel(void) override;

	const DWORD WaitTsStream(const DWORD dwTimeOut = 0) override;
	const DWORD GetReadyCount(void) override;

	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) override;
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) override;

	void PurgeTsStream(void) override;

// IBonDriver2(暫定)
	LPCWSTR GetTunerName(void) override;

	const BOOL IsTunerOpening(void) override;

	LPCWSTR EnumTuningSpace(const DWORD dwSpace) override;
	LPCWSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) override;

	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) override;

	const DWORD GetCurSpace(void) override;
	const DWORD GetCurChannel(void) override;

	void Release(void) override;

	static CBonTuner * m_pThis;
	static HINSTANCE m_hModule;
	static char * m_cList[7];


protected:
	// I/Oリクエストキューデータ
	struct AsyncIoReq
	{
		WSAOVERLAPPED OverLapped;
		DWORD dwState;
		DWORD dwRxdSize;
		BYTE RxdBuff[TSDATASIZE];
		AsyncIoReq *pNext;
	};

	AsyncIoReq * AllocIoReqBuff(const DWORD dwBuffNum);
	void FreeIoReqBuff(AsyncIoReq *pBuff);

	static DWORD WINAPI PushIoThread(LPVOID pParam);
	static DWORD WINAPI PopIoThread(LPVOID pParam);

	const BOOL PushIoRequest(SOCKET sock);
	const BOOL PopIoRequest(SOCKET sock);

	bool m_bTunerOpen;

	HANDLE m_hMutex;

	// SetChannel()/CloseTuner()の多重・再入呼び出しに対する排他制御
	// (同一スレッドからの再入はCRITICAL_SECTIONの性質上ブロックしない)
	CRITICAL_SECTION m_ChannelLock;

	AsyncIoReq *m_pIoReqBuff;
	AsyncIoReq *m_pIoPushReq;
	AsyncIoReq *m_pIoPopReq;
	AsyncIoReq *m_pIoGetReq;

	DWORD m_dwBusyReqNum;
	DWORD m_dwReadyReqNum;

	HANDLE m_hPushIoThread;
	HANDLE m_hPopIoThread;
	BOOL m_bLoopIoThread;

	HANDLE m_hOnStreamEvent;

	CRITICAL_SECTION m_CriticalSection;

	DWORD m_dwCurSpace;
	DWORD m_dwCurChannel;

	// 追加 byMeru(2008/03/27)
	SOCKET m_sock;
	float m_fBitRate;

	void CalcBitRate();
	void GetApiChannels(json &json_array, int service_split);
	DWORD m_dwRecvBytes;
	ULONGLONG m_u64LastCalcTick;
	ULONGLONG m_u64RecvBytes;
	ULONGLONG m_u64LastCalcByte;

#ifdef ENABLE_MMT4K
	// 現在選局中のチャンネルがMMT/TLV(4K/8K)かどうか。SetChannelで判定する。
	bool m_bMmtMode = false;
	std::unique_ptr<Mmt4kConverter> m_pMmt4kConverter;
	std::vector<uint8_t> m_MmtOutputBuffer;
#endif

};

#endif // !defined(_BONTUNER_H_)
