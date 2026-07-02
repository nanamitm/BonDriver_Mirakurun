#include <string>
#include <strsafe.h>

#include "BonDriver_Mirakurun.h"

#pragma comment(lib, "ws2_32.lib")

//////////////////////////////////////////////////////////////////////
// 定数定義
//////////////////////////////////////////////////////////////////////

#define BITRATE_CALC_TIME	500		//ms

// ミューテックス名
#define MUTEX_NAME			TEXT(TUNER_NAME)

// FIFOバッファ設定
#define ASYNCBUFFTIME		2											// バッファ長 = 2秒
#define ASYNCBUFFSIZE		( 0x200000 / TSDATASIZE * ASYNCBUFFTIME )	// 平均16Mbpsとする

#define REQRESERVNUM		8				// 非同期リクエスト予約数 //before 16
#define REQPOLLINGWAIT		20				// 非同期リクエストポーリング間隔(ms) //before 10

// 非同期リクエスト状態
#define IORS_IDLE			0x00			// リクエスト空
#define IORS_BUSY			0x01			// リクエスト受信中
#define IORS_RECV			0x02			// 受信完了、ストア待ち

// チャンネル切替時のサーバー接続リトライ設定
// (直前のチャンネルの旧接続がサーバー側でまだ解放されておらず、
//  同一優先度の新規ストリーム要求が一時的に拒否されるケースを吸収する)
#define CHANNEL_CONNECT_RETRY_MAX		5				// 最大リトライ回数
#define CHANNEL_CONNECT_RETRY_WAIT_MS	250				// リトライ間隔(ms)
#define HTTP_STATUS_TIMEOUT_MS			5000			// HTTPステータス受信タイムアウト(ms)


//////////////////////////////////////////////////////////////////////
// Tools
//////////////////////////////////////////////////////////////////////
// u8string -> wstring
std::wstring utf8_to_wstring(const std::u8string& utf8)
{
	int buf_size = ::MultiByteToWideChar(CP_UTF8, 0, (const char*)utf8.data(), -1, NULL, 0);// NULL文字を含むサイズ

	if (buf_size > 0)
	{
		std::wstring ret(buf_size - 1, '\0');

		// C++20から*(data()+size())をNULL文字で上書きすることが認められる
		if (::MultiByteToWideChar(CP_UTF8, 0, (const char*)utf8.data(), -1, ret.data(), buf_size) == buf_size) return ret;
	}
	return std::wstring();
}

// type名がg_MmtTypesに含まれるか(MMT/TLV=4K/8Kチャンネルかどうか)
static bool is_mmt_type(const std::string& type)
{
	return std::ranges::find(g_MmtTypes, type) != g_MmtTypes.end();
}

// url 生成
std::string make_channel_url(DWORD space, DWORD ch, int decode, json& data, bool* pIsMmt = nullptr)
{
	std::string url;


	if (pIsMmt) *pIsMmt = false;

	if (space < g_SpaceTypes.size() && ch < g_SpaceTypes[space].channel_num)
	{
		auto index = g_SpaceTypes[space].channel_base + ch;

		auto ch_json = data[index];

		if (!ch_json.is_null())
		{
			if (!ch_json.contains("serviceId")) // channels mode
			{
				auto type_json = ch_json["type"];
				if (type_json.is_string())
				{
					std::string type = ch_json["type"].get<std::string>();
					bool isMmt = is_mmt_type(type);
					if (pIsMmt) *pIsMmt = isMmt;
					int effectiveDecode = isMmt ? 0 : decode;

					auto channel_json = ch_json["channel"];
					if (channel_json.is_string()) // channelは文字列
					{
						url = "/api/channels/" + type + "/" + channel_json.get<std::string>() + "/stream?decode=" + std::to_string(effectiveDecode);
					}
				}
			}
			else if (ch_json.contains("channel")) // services mode
			{
				auto channel_detail = ch_json["channel"];

				auto type_json = channel_detail["type"];
				if (type_json.is_string())
				{
					std::string type = channel_detail["type"].get<std::string>();
					bool isMmt = is_mmt_type(type);
					if (pIsMmt) *pIsMmt = isMmt;
					int effectiveDecode = isMmt ? 0 : decode;

					auto channel_json = channel_detail["channel"];
					if (channel_json.is_string()) // channelは文字列
					{
						std::string channel = channel_detail["channel"].get<std::string>();

						auto sid_json = ch_json["serviceId"];
						if (sid_json.is_number())
						{
							url = "/api/channels/" + type + "/" + channel + "/services/" + std::to_string(sid_json.get<int>()) + "/stream?decode=" + std::to_string(effectiveDecode);
						}
					}
				}
			}
		}
	}
#ifdef _DEBUG
	char szDebugOut[256];
	::StringCbPrintfA(szDebugOut, _countof(szDebugOut), "%s: make_channel_url() url = %s\n", TUNER_NAME, url.c_str());
	::OutputDebugStringA(szDebugOut);
#endif
	return url;
}

// HTTPレスポンスのステータスコードを取得する
// (ヘッダ終端(\r\n\r\n)まで読み捨てる。ヘッダ以降に読み過ぎたTSデータは破棄するが、
//  ストリームは継続しているため後続のWSARecvで問題なく続きを受信できる)
// 戻り値: ステータスラインの受信・パースに成功したらtrue、タイムアウトや切断ならfalse
static bool RecvHttpStatusCode(SOCKET sock, int &statusCode, DWORD dwTimeoutMs)
{
	statusCode = 0;

	DWORD dwOldTimeout = 0;
	int nOldTimeoutLen = sizeof(dwOldTimeout);
	::getsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&dwOldTimeout, &nOldTimeoutLen);
	::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dwTimeoutMs, sizeof(dwTimeoutMs));

	std::string header;
	char buf[4096];
	bool bFoundEnd = false;

	// ヘッダ終端が見つかるまで受信する(異常に長いヘッダは打ち切る)
	while (header.size() < 16384) {
		int n = recv(sock, buf, sizeof(buf), 0);
		if (n <= 0) {
			break;
		}
		header.append(buf, n);
		if (header.find("\r\n\r\n") != std::string::npos) {
			bFoundEnd = true;
			break;
		}
	}

	::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dwOldTimeout, sizeof(dwOldTimeout));

	if (!bFoundEnd) {
		return false;
	}

	// ステータスライン("HTTP/1.1 200 OK")をパース
	size_t space1 = header.find(' ');
	if (space1 == std::string::npos) {
		return false;
	}

	statusCode = atoi(header.c_str() + space1 + 1);

	return statusCode != 0;
}


static int Init(HMODULE hModule)
{
	::GetModuleFileNameA(hModule, g_IniFilePath, _countof(g_IniFilePath));
	
	char* p = strrchr(g_IniFilePath, '.');
	if (!p) return -1;
	p++;
	strcpy_s(p, 16, "ini");

	HANDLE hFile = ::CreateFileA(g_IniFilePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return -2;
	
	::CloseHandle(hFile);

	::GetPrivateProfileStringA("GLOBAL", "SERVER_HOST", "localhost", g_ServerHost, _countof(g_ServerHost), g_IniFilePath);
	::GetPrivateProfileStringA("GLOBAL", "SERVER_PORT", "8888", g_ServerPort, _countof(g_ServerPort), g_IniFilePath);

	g_DecodeB25 = ::GetPrivateProfileIntA("GLOBAL", "DECODE_B25", 0, g_IniFilePath);
	g_Priority = ::GetPrivateProfileIntA("GLOBAL", "PRIORITY", 0, g_IniFilePath);
	g_Service_Split = ::GetPrivateProfileIntA("GLOBAL", "SERVICE_SPLIT", 0, g_IniFilePath);

	setlocale(LC_ALL, "japanese");

	{
		char szMmtTypes[256];
		::GetPrivateProfileStringA("GLOBAL", "MMT_TYPES", "BS4K", szMmtTypes, _countof(szMmtTypes), g_IniFilePath);
		g_MmtTypes.clear();
		char* pCtx = nullptr;
		for (char* p = strtok_s(szMmtTypes, ",", &pCtx); p; p = strtok_s(nullptr, ",", &pCtx)) {
			g_MmtTypes.push_back(p);
		}
	}

#ifdef ENABLE_MMT4K
	::GetPrivateProfileStringA("MMT4K", "SMARTCARD_READER_NAME", "", g_Mmt4kSmartCardReaderName, _countof(g_Mmt4kSmartCardReaderName), g_IniFilePath);
	::GetPrivateProfileStringA("MMT4K", "CASPROXY_SERVER", "", g_Mmt4kCasProxyServer, _countof(g_Mmt4kCasProxyServer), g_IniFilePath);
	::GetPrivateProfileStringA("MMT4K", "CUSTOM_WINSCARD_DLL", "", g_Mmt4kCustomWinscardDLL, _countof(g_Mmt4kCustomWinscardDLL), g_IniFilePath);
#endif

	g_MagicPacket_Enable = ::GetPrivateProfileIntA("GLOBAL", "MAGICPACKET_ENABLE", 0, g_IniFilePath);

	if (g_MagicPacket_Enable) {
		::GetPrivateProfileStringA("GLOBAL", "MAGICPACKET_TARGETMAC", "00:00:00:00:00:00", g_MagicPacket_TargetMAC, _countof(g_MagicPacket_TargetMAC), g_IniFilePath);

		for (int i = 0; i < 6; i++) {
			BYTE b = 0;
			char *p = &g_MagicPacket_TargetMAC[i * 3];
			for (int j = 0; j < 2; j++) {
				if ('0' <= *p && *p <= '9')
					b = b * 0x10 + (*p - '0');
				else if ('A' <= *p && *p <= 'F')
					b = b * 0x10 + (*p - 'A' + 10);
				else if ('a' <= *p && *p <= 'f')
					b = b * 0x10 + (*p - 'a' + 10);
				else
					return -2;
				p++;
			}
			g_MagicPacket_TargetMAC[i] = b;
		}

		::GetPrivateProfileStringA("GLOBAL", "MAGICPACKET_TARGETIP", "0.0.0.0", g_MagicPacket_TargetIP, _countof(g_MagicPacket_TargetIP), g_IniFilePath);
	}

	return 0;
}

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD fdwReason, LPVOID lpReserved)
{
	switch (fdwReason) {
		case DLL_PROCESS_ATTACH:
			if (Init(hModule) != 0) {
				return FALSE;
			}
			// モジュールハンドル保存
			CBonTuner::m_hModule = hModule;
			break;

		case DLL_PROCESS_DETACH:
			// 未開放の場合はインスタンス開放
			if (CBonTuner::m_pThis) {
				CBonTuner::m_pThis->Release();
			}
			break;
	}

	return TRUE;
}

inline ULONGLONG DiffTime(ULONGLONG BeginTime, ULONGLONG EndTime)
{
	if (BeginTime <= EndTime)
		return EndTime-BeginTime;
	return (ULLONG_MAX-BeginTime)+EndTime+1ULL;
}


//////////////////////////////////////////////////////////////////////
// インスタンス生成メソッド
//////////////////////////////////////////////////////////////////////

extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver()
{
	// スタンス生成(既存の場合はインスタンスのポインタを返す)
	return (CBonTuner::m_pThis)? CBonTuner::m_pThis : ((IBonDriver *) new CBonTuner);
}


//////////////////////////////////////////////////////////////////////
// 構築/消滅
//////////////////////////////////////////////////////////////////////

// 静的メンバ初期化
CBonTuner * CBonTuner::m_pThis = NULL;
HINSTANCE CBonTuner::m_hModule = NULL;

CBonTuner::CBonTuner()
	: m_bTunerOpen(FALSE)
	, m_hMutex(NULL)
	, m_pIoReqBuff(NULL)
	, m_pIoPushReq(NULL)
	, m_pIoPopReq(NULL)
	, m_pIoGetReq(NULL)
	, m_dwBusyReqNum(0UL)
	, m_dwReadyReqNum(0UL)
	, m_hPushIoThread(NULL)
	, m_hPopIoThread(NULL)
	, m_hOnStreamEvent(NULL)
	, m_dwCurSpace(0UL)
	, m_dwCurChannel(0xFFFFFFFFUL)
	, m_sock(INVALID_SOCKET)
	, m_fBitRate(0.0f)
	, m_dwRecvBytes(0UL)
	, m_u64LastCalcTick(0ULL)
{
	m_pThis = this;

	// クリティカルセクション初期化
	::InitializeCriticalSection(&m_CriticalSection);

	//Initialize channel
	InitChannel();
}

CBonTuner::~CBonTuner()
{
	// 開かれてる場合は閉じる
	CloseTuner();

	// クリティカルセクション削除
	::DeleteCriticalSection(&m_CriticalSection);

	// Winsock終了
	if (m_bTunerOpen) {
		WSACleanup();
	}

	m_pThis = NULL;
}

void CBonTuner::InitChannel()
{
	size_t elem_num = 0;

	GetApiChannels(g_Channel_JSON, g_Service_Split);

	g_SpaceTypes.clear();

	for (const auto& i : g_Channel_JSON)
	{
		std::u8string type_name;

		// チューナ空間名を取得
		if (!i.contains("serviceId")) // channels mode
		{
			auto type = i["type"];
			if (!type.is_null() && type.is_string())
			{
				type_name = (char8_t *)type.get<std::string>().c_str();
			}
		}
		else if (i.contains("channel")) // services mode
		{
			auto channel = i["channel"];

			auto type = channel["type"];
			if (!type.is_null() && type.is_string())
			{
				type_name = (char8_t *)type.get<std::string>().c_str();
			}
		}

		// typeが取得できないチャンネルは空間に含めずスキップする(elem_numは
		// g_Channel_JSON内の実インデックスと対応させる必要があるため、
		// スキップした場合も必ずインクリメントする)
		if (!type_name.empty())
		{
			auto it = std::ranges::find_if(g_SpaceTypes, [&](auto& e) { return e.name == type_name; });
			if (it != g_SpaceTypes.end())
			{
				(*it).channel_num++;
			}
			else// 同チューナ空間名が見つからない場合要素を追加
			{
				g_SpaceTypes.push_back(TSpaceType{ type_name, elem_num, 1 });
			}
		}
		elem_num++;
	}
}

const BOOL CBonTuner::OpenTuner()
{
	if (g_SpaceTypes.empty()) {
		return FALSE;
	}

	if (!m_bTunerOpen) {
		// Winsock初期化
		WSADATA stWsa;
		if (WSAStartup(MAKEWORD(2,2), &stWsa) != 0) {
			return FALSE;
		}
		if (g_MagicPacket_Enable) {
			char magicpacket[102];
			memset(&magicpacket, 0xff, sizeof(magicpacket));
			for (int i = 0; i < 16; i++) {
				memcpy(&magicpacket[i * 6 + 6], g_MagicPacket_TargetMAC, 6);
			}
			SOCKET s = socket(PF_INET, SOCK_DGRAM, 0);
			if (s == SOCKET_ERROR) {
				return FALSE;
			}
			SOCKADDR_IN addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(9);
			addr.sin_addr.S_un.S_addr = inet_addr(g_MagicPacket_TargetIP);

			sendto(s, magicpacket, sizeof(magicpacket), 0, (LPSOCKADDR)&addr, sizeof(addr));

			ULONGLONG LastTime = ::GetTickCount64();
			int countdown = 0;
			for (countdown = 0; countdown < MAGICPACKET_WAIT_SECONDS; countdown++) {
				try {
					struct addrinfo hints;
					struct addrinfo* res = NULL;
					struct addrinfo* ai;

					memset(&hints, 0, sizeof(hints));
					hints.ai_family = AF_INET6;	//IPv6優先
					hints.ai_socktype = SOCK_STREAM;
					hints.ai_protocol = IPPROTO_TCP;
					hints.ai_flags = AI_NUMERICSERV;
					if (getaddrinfo(g_ServerHost, g_ServerPort, &hints, &res) != 0) {
						//printf("getaddrinfo(): %s\n", gai_strerror(err));
						hints.ai_family = AF_INET;	//IPv4限定
						if (getaddrinfo(g_ServerHost, g_ServerPort, &hints, &res) != 0) {
							throw 1UL;
						}
					}

					for (ai = res; ai; ai = ai->ai_next) {
						m_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
						if (m_sock == INVALID_SOCKET) {
							continue;
						}

						if (connect(m_sock, ai->ai_addr, (int)ai->ai_addrlen) >= 0) {
							// OK
							break;
						}
						closesocket(m_sock);
						m_sock = INVALID_SOCKET;
					}
					freeaddrinfo(res);

					if (m_sock == INVALID_SOCKET) {
						TCHAR szDebugOut[128];
						::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::OpenTuner() connection error %d\n"), TEXT(TUNER_NAME), WSAGetLastError());
						::OutputDebugString(szDebugOut);
						throw 1UL;
					}

					const char serverRequest[] = "GET / HTTP/1.0\r\n\r\n";
					if (send(m_sock, serverRequest, (int)strlen(serverRequest), 0) < 0) {
						TCHAR szDebugOut[128];
						::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::OpenTuner() send error %d\n"), TEXT(TUNER_NAME), WSAGetLastError());
						::OutputDebugString(szDebugOut);
						throw 1UL;
					}
					// Success
					break;
				}
				catch (const DWORD dwErrorStep) {
					if (::GetTickCount64() - LastTime > (MAGICPACKET_WAIT_SECONDS * 1000)) {
						TCHAR szDebugOut[1024];
						::wsprintf(szDebugOut, TEXT("TimeOut\n"));
						::OutputDebugString(szDebugOut);
						return FALSE;
					}
					// エラー発生
					TCHAR szDebugOut[1024];
					::StringCbPrintf(szDebugOut, _countof(szDebugOut),  TEXT("%s: CBonTuner::OpenTuner() dwErrorStep = %lu\n)"), TEXT(TUNER_NAME), dwErrorStep);
					::OutputDebugString(szDebugOut);
					Sleep(1000);
				}
			}

			if (countdown >= MAGICPACKET_WAIT_SECONDS) {
				// Failed
				return FALSE;
			}
		}

		m_bTunerOpen = TRUE;
	}

	//return SetChannel(0UL,0UL);
	
	return TRUE;
}

void CBonTuner::CloseTuner()
{
	// スレッド終了要求セット
	m_bLoopIoThread = FALSE;

	// サーバーへの切断通知を最優先で行う
	// (スレッド終了やハンドル開放より前にshutdown()でFINを即座に送出することで、
	//  万一スレッド終了処理が詰まった場合でも、サーバー側には速やかに切断が伝わるようにする。
	//  また、未読の受信データが残った状態でいきなりclosesocket()するとRSTが送出され
	//  サーバー側が切断を検知できないことがあるため、先に正常なshutdown()を行う)
	if (m_sock != INVALID_SOCKET) {
		::shutdown(m_sock, SD_BOTH);

		// 保留中の非同期WSARecvを強制キャンセルし、Push/PopIoThreadが速やかに
		// ループを抜けられるようにする(TerminateThreadでの強制終了は
		// Winsock内部状態を破壊しソケットが正しく閉じられなくなる恐れがあるため、
		// できる限り使わずに済むようにする)
		::CancelIoEx((HANDLE)m_sock, NULL);
	}

	// イベント開放
	if (m_hOnStreamEvent) {
		::CloseHandle(m_hOnStreamEvent);
		m_hOnStreamEvent = NULL;
	}

	// スレッド終了
	if (m_hPushIoThread) {
		if (::WaitForSingleObject(m_hPushIoThread, 1000) != WAIT_OBJECT_0) {
			// スレッド強制終了
#pragma warning(push)
#pragma warning(disable:6258)
			::TerminateThread(m_hPushIoThread, 0);
#pragma warning(pop)

			TCHAR szDebugOut[128];
			::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::CloseTuner() ::TerminateThread(m_hPushIoThread)\n"), TEXT(TUNER_NAME));
			::OutputDebugString(szDebugOut);
		}

		::CloseHandle(m_hPushIoThread);
		m_hPushIoThread = NULL;
	}

	if (m_hPopIoThread) {
		if (::WaitForSingleObject(m_hPopIoThread, 1000) != WAIT_OBJECT_0) {
			// スレッド強制終了
#pragma warning(push)
#pragma warning(disable:6258)
			::TerminateThread(m_hPopIoThread, 0);
#pragma warning(pop)
			TCHAR szDebugOut[128];
			::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::CloseTuner() ::TerminateThread(m_hPopIoThread)\n"), TEXT(TUNER_NAME));
			::OutputDebugString(szDebugOut);
		}

		::CloseHandle(m_hPopIoThread);
		m_hPopIoThread = NULL;
	}

	// バッファ開放
	FreeIoReqBuff(m_pIoReqBuff);
	m_pIoReqBuff = NULL;
	m_pIoPushReq = NULL;
	m_pIoPopReq = NULL;
	m_pIoGetReq = NULL;

	m_dwBusyReqNum = 0UL;
	m_dwReadyReqNum = 0UL;

	// ソケットクローズ
	if (m_sock != INVALID_SOCKET) {
		if (closesocket(m_sock) == SOCKET_ERROR) {
			TCHAR szDebugOut[128];
			::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::CloseTuner() closesocket error %d\n"), TEXT(TUNER_NAME), WSAGetLastError());
			::OutputDebugString(szDebugOut);
		}
		m_sock = INVALID_SOCKET;
	}

	// チャンネル初期化
	m_dwCurSpace = 0UL;
	m_dwCurChannel = 0xFFFFFFFFUL;

	// ミューテックス開放
	if (m_hMutex) {
		::ReleaseMutex(m_hMutex);
		::CloseHandle(m_hMutex);
		m_hMutex = NULL;
	}

	m_fBitRate = 0.0f;
	m_dwRecvBytes = 0UL;
}

const DWORD CBonTuner::WaitTsStream(const DWORD dwTimeOut)
{
	// 終了チェック
	if (!m_hOnStreamEvent || !m_bLoopIoThread) {
		return WAIT_ABANDONED;
	}

	// イベントがシグナル状態になるのを待つ
	const DWORD dwRet = ::WaitForSingleObject(m_hOnStreamEvent, (dwTimeOut)? dwTimeOut : INFINITE);

	switch (dwRet) {
		case WAIT_ABANDONED :
			// チューナが閉じられた
			return WAIT_ABANDONED;

		case WAIT_OBJECT_0 :
		case WAIT_TIMEOUT :
			// ストリーム取得可能 or チューナが閉じられた
			return (m_bLoopIoThread)? dwRet : WAIT_ABANDONED;

		case WAIT_FAILED :
		default:
			// 例外
			return WAIT_FAILED;
	}
}

const DWORD CBonTuner::GetReadyCount()
{
	// 取り出し可能TSデータ数を取得する
	return m_dwReadyReqNum;
}

const BOOL CBonTuner::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	BYTE *pSrc = NULL;

	// TSデータをバッファから取り出す
	if (GetTsStream(&pSrc, pdwSize, pdwRemain)) {
		if (*pdwSize) {
			::CopyMemory(pDst, pSrc, *pdwSize);
		}

		return TRUE;
	}

	return FALSE;
}

const BOOL CBonTuner::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!m_pIoGetReq) {
		return FALSE;
	}

	// TSデータをバッファから取り出す
	if (m_dwReadyReqNum) {
		if (m_pIoGetReq->dwState == IORS_RECV) {

			// データコピー
			BYTE* pRawData = m_pIoGetReq->RxdBuff;
			DWORD dwRawSize = m_pIoGetReq->dwRxdSize;

			// バッファ位置を進める
			::EnterCriticalSection(&m_CriticalSection);
			m_pIoGetReq = m_pIoGetReq->pNext;
			m_dwReadyReqNum--;
			*pdwRemain = m_dwReadyReqNum;
			::LeaveCriticalSection(&m_CriticalSection);

#ifdef ENABLE_MMT4K
			// MMT/TLV(4K/8K)チャンネルの場合はMmt4kConverterでMPEG2-TSに変換してから返す
			if (m_bMmtMode && m_pMmt4kConverter) {
				m_pMmt4kConverter->Push(pRawData, dwRawSize);
				m_MmtOutputBuffer = m_pMmt4kConverter->TakeOutput();

				*ppDst = m_MmtOutputBuffer.data();
				*pdwSize = static_cast<DWORD>(m_MmtOutputBuffer.size());

				return TRUE;
			}
#endif

			*ppDst = pRawData;
			*pdwSize = dwRawSize;

			return TRUE;
		}

		// 例外
		return FALSE;
	}

	// 取り出し可能なデータがない
	*pdwSize = 0;
	*pdwRemain = 0;

	return TRUE;
}

void CBonTuner::PurgeTsStream()
{
	// バッファから取り出し可能データをパージする
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoGetReq = m_pIoPopReq;
	m_dwReadyReqNum = 0;
	::LeaveCriticalSection(&m_CriticalSection);

#ifdef ENABLE_MMT4K
	if (m_bMmtMode && m_pMmt4kConverter) {
		m_pMmt4kConverter->Reset();
	}
	m_MmtOutputBuffer.clear();
#endif
}

void CBonTuner::Release()
{
	// インスタンス開放
	delete this;
}

LPCWSTR CBonTuner::GetTunerName(void)
{
	// チューナ名を返す
	return TUNER_NAME_W;
}

const BOOL CBonTuner::IsTunerOpening(void)
{
	// チューナの使用中の有無を返す(全プロセスを通して)
	HANDLE hMutex = ::OpenMutex(MUTEX_ALL_ACCESS, FALSE, MUTEX_NAME);

	if (hMutex) {
		// 既にチューナは開かれている
		::CloseHandle(hMutex);
		return TRUE;
	}

	// チューナは開かれていない
	return FALSE;
}

// 使用可能なチューニング空間を返す
LPCWSTR CBonTuner::EnumTuningSpace(const DWORD dwSpace)
{
	static std::wstring wSpaceName;
	
	if (dwSpace >= g_SpaceTypes.size()) {
		return NULL;
	}

	wSpaceName = utf8_to_wstring(g_SpaceTypes[dwSpace].name);

	return wSpaceName.c_str();
}

LPCWSTR CBonTuner::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if (dwSpace < g_SpaceTypes.size() && dwChannel < g_SpaceTypes[dwSpace].channel_num)
	{
		static std::wstring wChannelName;
		auto index = g_SpaceTypes[dwSpace].channel_base + dwChannel;

		auto ch_json = g_Channel_JSON[index];

		if (!ch_json.is_null())
		{
			auto name = ch_json["name"];

			if (name.is_string())
			{
				wChannelName = utf8_to_wstring((char8_t *)name.get<std::string>().c_str());
	
				return wChannelName.c_str();
			}
		}
	}
	return NULL;
}

const DWORD CBonTuner::GetCurSpace(void)
{
	// 現在のチューニング空間を返す
	return m_dwCurSpace;
}

const DWORD CBonTuner::GetCurChannel(void)
{
	// 現在のチャンネルを返す
	return m_dwCurChannel;
}

CBonTuner::AsyncIoReq * CBonTuner::AllocIoReqBuff(const DWORD dwBuffNum)
{
	if (dwBuffNum < 2) {
		return NULL;
	}

	// メモリを確保する
	AsyncIoReq *pNewBuff = new AsyncIoReq [dwBuffNum];
	if (!pNewBuff) {
		return NULL;
	}

	// ゼロクリア
	::ZeroMemory(pNewBuff, sizeof(AsyncIoReq) * dwBuffNum);

	// リンクを構築する
	DWORD dwIndex;
	for(dwIndex = 0 ; dwIndex < ( dwBuffNum - 1 ) ; dwIndex++) {
		pNewBuff[dwIndex].pNext= &pNewBuff[dwIndex + 1];
	}

	pNewBuff[dwIndex].pNext = &pNewBuff[0];

	return pNewBuff;
}

void CBonTuner::FreeIoReqBuff(CBonTuner::AsyncIoReq *pBuff)
{
	if (!pBuff) {
		return;
	}

	// バッファを開放する
	delete [] pBuff;
}

DWORD WINAPI CBonTuner::PushIoThread(LPVOID pParam)
{
	CBonTuner *pThis = (CBonTuner *)pParam;

#ifdef _DEBUG
	::OutputDebugString(TEXT("CBonTuner::PushIoThread() Start!\n"));
#endif
	// ドライバにTSデータリクエストを発行する
	while (pThis->m_bLoopIoThread) {

		// リクエスト処理待ちが規定未満なら追加する
		if (pThis->m_dwBusyReqNum < REQRESERVNUM) {

			// ドライバにTSデータリクエストを発行する(HTTPなので受信要求のみ)
			if (!pThis->PushIoRequest(pThis->m_sock)) {
				// エラー発生
				break;
			}

		} else {
			// リクエスト処理待ちがフルの場合はウェイト
			::Sleep(REQPOLLINGWAIT);
		}
	}
#ifdef _DEBUG
	::OutputDebugString(TEXT("CBonTuner::PushIoThread() End!\n"));
#endif
	return 0;
}

DWORD WINAPI CBonTuner::PopIoThread(LPVOID pParam)
{
	CBonTuner *pThis = (CBonTuner *)pParam;

	// 処理済リクエストをポーリングしてリクエストを完了させる
	while (pThis->m_bLoopIoThread) {

		// 処理済データがあればリクエストを完了する
		if (pThis->m_dwBusyReqNum) {

			// リクエストを完了する
			if (!pThis->PopIoRequest(pThis->m_sock)) {
				// エラー発生
				break;
			}
		}
	}

	return 0;
}

const BOOL CBonTuner::PushIoRequest(SOCKET sock)
{
	// ドライバに非同期リクエストを発行する

	// オープンチェック
	if (sock == INVALID_SOCKET)return FALSE;

	// リクエストセット
	m_pIoPushReq->dwRxdSize = 0;

	// イベント設定
	::ZeroMemory(&m_pIoPushReq->OverLapped, sizeof(WSAOVERLAPPED));
	if (!(m_pIoPushReq->OverLapped.hEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL)))return FALSE;

	// HTTP受信を要求スルニダ！
	DWORD Flags = 0;
	WSABUF wsaBuf{};
	wsaBuf.buf = (char *)m_pIoPushReq->RxdBuff;
	wsaBuf.len = sizeof(m_pIoPushReq->RxdBuff);
	if (SOCKET_ERROR == WSARecv(sock, &wsaBuf, 1, &m_pIoPushReq->dwRxdSize, &Flags, &m_pIoPushReq->OverLapped, NULL)) {
		int sock_err = WSAGetLastError();
		if (sock_err != ERROR_IO_PENDING) {
			return FALSE;
		}
	}

	m_pIoPushReq->dwState = IORS_BUSY;

	// バッファ状態更新
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoPushReq = m_pIoPushReq->pNext;
	m_dwBusyReqNum++;
	::LeaveCriticalSection(&m_CriticalSection);

	return TRUE;
}

const BOOL CBonTuner::PopIoRequest(SOCKET sock)
{
	// 非同期リクエストを完了する

	// オープンチェック
	if (sock == INVALID_SOCKET) {
		return FALSE;
	}

	// 状態チェック
	if (m_pIoPopReq->dwState != IORS_BUSY) {
		// 例外
		return TRUE;
	}

	// リクエスト取得
	DWORD Flags=0;
	const BOOL bRet = ::WSAGetOverlappedResult(sock, &m_pIoPopReq->OverLapped, &m_pIoPopReq->dwRxdSize, FALSE, &Flags);

	// エラーチェック
	if (!bRet) {
		int sock_err = WSAGetLastError();
		if (sock_err == ERROR_IO_INCOMPLETE) {
			// 処理未完了
			::Sleep(REQPOLLINGWAIT);
			return TRUE;
		}
	}

	// 総受信サイズ加算
	m_dwRecvBytes += m_pIoPopReq->dwRxdSize;

	// ビットレート計算
	if (DiffTime(m_u64LastCalcTick,::GetTickCount64()) >= BITRATE_CALC_TIME) {
		CalcBitRate();
	}

	// イベント削除
	::CloseHandle(m_pIoPopReq->OverLapped.hEvent);

	if (!bRet) {
		// エラー発生
		return FALSE;
	}

	m_pIoPopReq->dwState = IORS_RECV;

	// バッファ状態更新
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoPopReq = m_pIoPopReq->pNext;
	m_dwBusyReqNum--;
	m_dwReadyReqNum++;
	::LeaveCriticalSection(&m_CriticalSection);

	// イベントセット
	::SetEvent(m_hOnStreamEvent);

	return TRUE;
}


// チャンネル設定
const BOOL CBonTuner::SetChannel(const BYTE bCh)
{
	return SetChannel((DWORD)0,(DWORD)bCh - 13);
}


// チャンネル設定
const BOOL CBonTuner::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	bool bIsMmt = false;
	std::string url = make_channel_url(dwSpace, dwChannel, g_DecodeB25, g_Channel_JSON, &bIsMmt);

	if (url.empty()) return FALSE;

	// 一旦クローズ
	CloseTuner();

#ifdef ENABLE_MMT4K
	m_bMmtMode = bIsMmt;
	if (m_bMmtMode) {
		if (!m_pMmt4kConverter) {
			m_pMmt4kConverter = std::make_unique<Mmt4kConverter>();
		}
		if (!m_pMmt4kConverter->Init(g_Mmt4kSmartCardReaderName, g_Mmt4kCasProxyServer, g_Mmt4kCustomWinscardDLL)) {
			return FALSE;
		}
		m_pMmt4kConverter->Reset();
	}
	m_MmtOutputBuffer.clear();
#endif

	// バッファ確保
	if (!(m_pIoReqBuff = AllocIoReqBuff(ASYNCBUFFSIZE))) {
		return FALSE;
	}

	// バッファ位置同期
	m_pIoPushReq = m_pIoReqBuff;
	m_pIoPopReq = m_pIoReqBuff;
	m_pIoGetReq = m_pIoReqBuff;
	m_dwBusyReqNum = 0;
	m_dwReadyReqNum = 0;

	try{
		std::string serverRequest = "GET " + url +" HTTP/1.0\r\nX-Mirakurun-Priority: " + std::to_string(g_Priority) + "\r\n\r\n";

		// サーバー側で直前のチャンネルの旧接続がまだ解放されておらず、
		// 同一優先度の新規ストリーム要求が一時的に拒否される(200以外が返る)ことがあるため、
		// 失敗時は少し待ってリトライする
		int httpStatus = 0;
		for (int attempt = 1; attempt <= CHANNEL_CONNECT_RETRY_MAX; attempt++) {
			struct addrinfo hints;
			struct addrinfo* res = NULL;
			struct addrinfo* ai;

			memset(&hints, 0, sizeof(hints));
			hints.ai_family = AF_INET6;	//IPv6優先
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;
			hints.ai_flags = AI_NUMERICSERV;
			if (getaddrinfo(g_ServerHost, g_ServerPort, &hints, &res) != 0) {
				//printf("getaddrinfo(): %s\n", gai_strerror(err));
				hints.ai_family = AF_INET;	//IPv4限定
				if (getaddrinfo(g_ServerHost, g_ServerPort, &hints, &res) != 0) {
					throw 1UL;
				}
			}

			for (ai = res; ai; ai = ai->ai_next) {
				m_sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
				if (m_sock == INVALID_SOCKET) {
					continue;
				}

				if (connect(m_sock, ai->ai_addr, (int)ai->ai_addrlen) >= 0) {
					// OK
					break;
				}
				closesocket(m_sock);
				m_sock = INVALID_SOCKET;
			}
			freeaddrinfo(res);

			if (m_sock == INVALID_SOCKET) {
				TCHAR szDebugOut[128];
				::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::OpenTuner() connection error %d\n"), TEXT(TUNER_NAME), WSAGetLastError());
				::OutputDebugString(szDebugOut);
				throw 1UL;
			}

			if (send(m_sock, serverRequest.c_str(), (int)serverRequest.length(), 0) < 0) {
				TCHAR szDebugOut[128];
				::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::OpenTuner() send error %d\n"), TEXT(TUNER_NAME), WSAGetLastError());
				::OutputDebugString(szDebugOut);
				throw 1UL;
			}

			// レスポンスのステータスコードを確認する
			// (チューナー未解放等により200以外が返る場合はリトライする)
			if (RecvHttpStatusCode(m_sock, httpStatus, HTTP_STATUS_TIMEOUT_MS) && httpStatus == 200) {
				break;
			}

			TCHAR szDebugOut[192];
			::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::SetChannel() http status = %d (attempt %d/%d)\n"), TEXT(TUNER_NAME), httpStatus, attempt, CHANNEL_CONNECT_RETRY_MAX);
			::OutputDebugString(szDebugOut);

			closesocket(m_sock);
			m_sock = INVALID_SOCKET;

			if (attempt == CHANNEL_CONNECT_RETRY_MAX) {
				throw 6UL;
			}

			::Sleep(CHANNEL_CONNECT_RETRY_WAIT_MS);
		}

		// イベント作成
		if (!(m_hOnStreamEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL))) {
			throw 2UL;
		}

		// スレッド起動
		DWORD dwPushIoThreadID = 0UL, dwPopIoThreadID = 0UL;
		m_hPushIoThread = ::CreateThread(NULL, 0UL, CBonTuner::PushIoThread, this, CREATE_SUSPENDED, &dwPopIoThreadID);
		m_hPopIoThread = ::CreateThread(NULL, 0UL, CBonTuner::PopIoThread, this, CREATE_SUSPENDED, &dwPushIoThreadID);

		if (!m_hPushIoThread || !m_hPopIoThread) {
			if (m_hPushIoThread) {
#pragma warning(push)
#pragma warning(disable:6258)
				::TerminateThread(m_hPushIoThread, 0UL);
#pragma warning(pop)

				::CloseHandle(m_hPushIoThread);
				m_hPushIoThread = NULL;
			}

			if (m_hPopIoThread) {
#pragma warning(push)
#pragma warning(disable:6258)
				::TerminateThread(m_hPopIoThread, 0UL);
#pragma warning(pop)
				::CloseHandle(m_hPopIoThread);
				m_hPopIoThread = NULL;
			}

			throw 3UL;
		}

		// スレッド開始
		m_bLoopIoThread = TRUE;
		if (::ResumeThread(m_hPushIoThread) == 0xFFFFFFFFUL || ::ResumeThread(m_hPopIoThread) == 0xFFFFFFFFUL) {
			throw 4UL;
		}

		// ミューテックス作成
		if (!(m_hMutex = ::CreateMutex(NULL, TRUE, MUTEX_NAME))) {
			throw 5UL;
		}

	} catch (const DWORD dwErrorStep) {
		// エラー発生
		TCHAR szDebugOut[1024];
		::StringCbPrintf(szDebugOut, _countof(szDebugOut), TEXT("%s: CBonTuner::OpenTuner() dwErrorStep = %lu\n"), TEXT(TUNER_NAME), dwErrorStep);
		::OutputDebugString(szDebugOut);

		CloseTuner();
		return FALSE;
	}

	// チャンネル情報更新
	m_dwCurSpace = dwSpace;
	m_dwCurChannel = dwChannel;

	// TSデータパージ
	PurgeTsStream();

	return TRUE;
}

// 信号レベル(ビットレート)取得
const float CBonTuner::GetSignalLevel(void)
{
	CalcBitRate();
	return m_fBitRate;
}

void CBonTuner::CalcBitRate()
{
	ULONGLONG u64CurrentTick = ::GetTickCount64();
	ULONGLONG u64Span = DiffTime(m_u64LastCalcTick, u64CurrentTick);

	if (u64Span >= BITRATE_CALC_TIME) {
		m_fBitRate = (float)(((double)m_dwRecvBytes*(8*1000))/((double)u64Span*(1024*1024)));
		m_dwRecvBytes = 0;
		m_u64LastCalcTick = u64CurrentTick;
	}
	return;
}

void CBonTuner::GetApiChannels(json& json_array, int service_split)
{
	HttpClient client;
	std::string url = "http://" + std::string(g_ServerHost) + ":" + std::string(g_ServerPort);

	url += (service_split == 1) ? "/api/services" : "/api/channels";

	HttpResponse response = client.get(url);

	json_array = json::parse(response.content);
}
