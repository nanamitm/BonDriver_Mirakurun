// Production driver control flow, with only the external converter/recorder replaced.
#define ENABLE_MMT4K
#include "../BonDriver_Mirakurun.cpp"
#include <thread>
#include <iostream>
#include <functional>
#include <cstdlib>

static std::atomic<unsigned> pushes{0}, resets{0};
static std::atomic<bool> slowPush{false}, pushFinished{false};
struct Mmt4kConverter::Impl {};
Mmt4kConverter::Mmt4kConverter() : impl(std::make_unique<Impl>()) {}
Mmt4kConverter::~Mmt4kConverter() = default;
bool Mmt4kConverter::Init(const std::string&, const std::string&, const std::string&, bool, bool) { return true; }
void Mmt4kConverter::Push(const uint8_t*, size_t) {
    ++pushes;
    if (slowPush) Sleep(1200);
    pushFinished = true;
}
std::vector<uint8_t> Mmt4kConverter::TakeOutput() { return std::vector<uint8_t>(188, 0x47); }
void Mmt4kConverter::Reset() { ++resets; }
namespace MmtsRecorder {
    bool Start(const wchar_t*, bool, uint32_t*) { return false; }
    void Stop(uint32_t) {}
    bool GetStatus(uint32_t, uint32_t*, bool*, bool*) { return false; }
}
static unsigned checks = 0;
static void check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
static bool waitUntil(const std::function<bool()>& predicate, DWORD timeout = 3000) {
    const ULONGLONG end = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(1); } while (GetTickCount64() < end);
    return false;
}
static std::string response(const std::string& body) {
    return "HTTP/1.0 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
struct Server {
    SOCKET listener;
    unsigned short port;
    std::thread worker;
    Server(std::vector<std::string> replies, bool requests = true, bool fragment = false, DWORD hold = 0) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::bind(listener, (sockaddr*)&address, sizeof(address)) == 0, "bind fixture");
        check(listen(listener, 8) == 0, "listen fixture");
        int size = sizeof(address);
        getsockname(listener, (sockaddr*)&address, &size);
        port = ntohs(address.sin_port);
        worker = std::thread([this, replies, requests, fragment, hold] {
            for (const auto& reply : replies) {
                fd_set set; FD_ZERO(&set); FD_SET(listener, &set);
                timeval timeout{5, 0};
                if (select(0, &set, nullptr, nullptr, &timeout) <= 0) return;
                SOCKET client = accept(listener, nullptr, nullptr);
                DWORD ioTimeout = 2000;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&ioTimeout, sizeof(ioTimeout));
                if (requests) {
                    std::string request; char buf[1024];
                    while (request.find("\r\n\r\n") == std::string::npos) {
                        int n = recv(client, buf, sizeof(buf), 0);
                        if (n <= 0) break;
                        request.append(buf, n);
                    }
                }
                size_t sent = 0;
                while (sent < reply.size()) {
                    int n = send(client, reply.data() + sent, fragment ? 1 : (int)(reply.size() - sent), 0);
                    if (n <= 0) break;
                    sent += n;
                    if (fragment) Sleep(1);
                }
                if (hold) Sleep(hold);
                shutdown(client, SD_SEND);
                closesocket(client);
            }
        });
    }
    ~Server() { worker.join(); closesocket(listener); }
    void configure(const char* host = "127.0.0.1") {
        strcpy_s(g_ServerHost, host); sprintf_s(g_ServerPort, "%u", port);
        g_Service_Split = 0; g_MagicPacket_Enable = 0;
    }
    SOCKET connectClient() {
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        check(connect(client, (sockaddr*)&address, sizeof(address)) == 0, "connect fixture");
        return client;
    }
};
// Keep tests on ordinary C++ access rules; expose protected driver state for race fixtures.
struct TestTuner : CBonTuner {
    using CBonTuner::m_CriticalSection;
    using CBonTuner::m_MmtLock;
    using CBonTuner::m_MmtGeneration;
    using CBonTuner::m_pIoReqBuff;
    using CBonTuner::m_pIoGetReq;
    using CBonTuner::m_pIoPopReq;
    using CBonTuner::m_dwReadyReqNum;
    using CBonTuner::m_MmtOutputQueue;
    void prepareMmt() {
        m_pIoReqBuff = AllocIoReqBuff(ASYNCBUFFSIZE);
        m_pIoGetReq = m_pIoReqBuff;
        m_pIoPopReq = m_pIoReqBuff + 1;
        m_pIoReqBuff->dwState = IORS_RECV;
        m_pIoReqBuff->dwRxdSize = 1;
        m_dwReadyReqNum = 1;
        m_bMmtMode = true;
        m_pMmt4kConverter = std::make_unique<Mmt4kConverter>();
        m_hStartIoEvent = CreateEvent(nullptr, TRUE, TRUE, nullptr);
        m_bLoopIoThread = true;
        m_hMmtConvertThread = CreateThread(nullptr, 0, MmtConvertThread, this, 0, nullptr);
        check(m_hMmtConvertThread != nullptr, "start converter fixture");
    }
    void preparePending() {
        m_pIoReqBuff = AllocIoReqBuff(ASYNCBUFFSIZE);
        m_pIoReqBuff->dwState = IORS_BUSY;
        m_pIoReqBuff->OverLapped.Internal = 0x103; // STATUS_PENDING
        m_pIoReqBuff->OverLapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    }
    void prepareGatedWorker() {
        m_hStartIoEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        m_hPushIoThread = CreateThread(nullptr, 0, PushIoThread, this, 0, nullptr);
        check(m_hPushIoThread != nullptr, "start gated worker fixture");
    }
};
int main() {
    WSADATA wsa; check(WSAStartup(MAKEWORD(2,2), &wsa) == 0, "Winsock startup");
    for (bool fragment : {false, true}) {
        Server server({"HTTP/1.0 200 OK\r\nX-Test: yes\r\n\r\nABCD"}, false, fragment);
        SOCKET client = server.connectClient(); int status;
        check(RecvHttpStatusCode(client, status, 1000) && status == 200, "header status");
        std::string body; char buf[16]; int n;
        while ((n = recv(client, buf, sizeof(buf), 0)) > 0) body.append(buf, n);
        check(body == "ABCD", "preserve coalesced/fragmented stream body");
        closesocket(client);
    }
    {
        Server server({"HTTP/1.0 200 OK\r\nX-Test: incomplete"}, false, false, 100);
        SOCKET client = server.connectClient(); int status;
        check(!RecvHttpStatusCode(client, status, 20), "incomplete header timeout");
        closesocket(client);
    }
    struct Case { const char* header; const char* body; bool valid; const char* decoded; };
    const Case cases[] = {
        {"Content-Length: 2", "[]", true, "[]"},
        {"Content-Length: 100", "[]", false, ""},
        {"Content-Length: -1", "[]", false, ""},
        {"Content-Length: 2x", "[]", false, ""},
        {"Content-Length: 999999999999999999999999", "[]", false, ""},
        {"Transfer-Encoding: chunked", "2\r\n[]\r\n0\r\n\r\n", true, "[]"},
        {"Transfer-Encoding: chunked", "2;key=value\r\n[]\r\n0\r\nX-Test: yes\r\n\r\n", true, "[]"},
        {"Transfer-Encoding: chunked", "2\r\n[]\r\n", false, ""},
        {"Transfer-Encoding: chunked", "2\r\n[]xx0\r\n\r\n", false, ""},
        {"Transfer-Encoding: chunked", "2x\r\n[]\r\n0\r\n\r\n", false, ""},
        {"Transfer-Encoding: chunked", "0\r\n", false, ""},
        {"Transfer-Encoding: chunked", "ffffffffffffffffffffffff\r\n", false, ""},
        {"Transfer-Encoding: gzip\r\nX-Test: chunked", "[]", false, ""},
    };
    for (const auto& test : cases) {
        Server server({std::string("HTTP/1.0 200 OK\r\n") + test.header + "\r\n\r\n" + test.body});
        server.configure(); int status; std::string body;
        const bool valid = HttpGet("/api/channels", status, body, 1000, 1000);
        check(valid == test.valid, "HTTP framing acceptance");
        check(body == test.decoded, "HTTP complete body or empty failure");
    }
    {
        Server server({"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n[]"}, true, false, 150);
        server.configure(); int status; std::string body;
        check(!HttpGet("/api/channels", status, body, 1000, 30), "receive timeout is not successful EOF");
    }
    {
        std::string ts(188 * 16, '\0');
        for (size_t pos = 0; pos < ts.size(); pos += 188) ts[pos] = 0x47;
        Server server({response("[{\"type\":\"GR\",\"channel\":\"1\",\"name\":\"test\"}]"), response(ts)});
        server.configure("localhost"); TestTuner tuner;
        check(g_Channel_JSON.size() == 1, "discover IPv4-only localhost channels");
        check(tuner.SetChannel(0UL, 0UL), "tune IPv4-only localhost");
        std::string actual;
        check(waitUntil([&] {
            BYTE* data; DWORD size, remain;
            if (tuner.GetTsStream(&data, &size, &remain) && size) actual.append((char*)data, size);
            return actual.size() >= ts.size();
        }), "deliver initial stream data");
        check(actual == ts, "initial stream bytes unchanged");
        tuner.CloseTuner();
    }
    {
        Server server({response("[]")}); server.configure(); TestTuner tuner;
        tuner.prepareGatedWorker(); tuner.CloseTuner();
        check(tuner.m_pIoReqBuff == nullptr, "stop partially started gated worker");
    }
    {
        Server server({response("[]")}); server.configure(); TestTuner tuner;
        pushes = 0; resets = 0; slowPush = false;
        EnterCriticalSection(&tuner.m_MmtLock);
        tuner.prepareMmt();
        check(waitUntil([&] { CAutoLock lock(tuner.m_CriticalSection); return tuner.m_dwReadyReqNum == 0; }), "dequeue before purge");
        const uint64_t generation = tuner.m_MmtGeneration;
        std::thread purge([&] { tuner.PurgeTsStream(); });
        check(waitUntil([&] { return tuner.m_MmtGeneration != generation; }), "purge generation advanced");
        LeaveCriticalSection(&tuner.m_MmtLock); purge.join();
        Sleep(20);
        check(pushes == 0 && resets == 1, "pre-purge dequeued input discarded");
        {
            CAutoLock lock(tuner.m_CriticalSection);
            tuner.m_pIoGetReq->dwState = IORS_RECV;
            tuner.m_pIoGetReq->dwRxdSize = 1;
            tuner.m_dwReadyReqNum = 1;
        }
        check(waitUntil([&] { return pushes == 1; }), "post-purge input converted");
        tuner.CloseTuner();
    }
    {
        Server server({response("[]")}); server.configure(); TestTuner tuner;
        pushes = 0; slowPush = true; pushFinished = false;
        tuner.prepareMmt(); check(waitUntil([] { return pushes != 0; }), "slow converter entered");
        tuner.CloseTuner(); check(pushFinished, "shutdown joins conversion longer than one second");
        check(TryEnterCriticalSection(&tuner.m_MmtLock) != 0, "converter lock released normally");
        LeaveCriticalSection(&tuner.m_MmtLock); slowPush = false;
    }
    {
        Server server({response("[]")}); server.configure(); TestTuner tuner;
        tuner.preparePending(); auto* slot = tuner.m_pIoReqBuff;
        HANDLE notification;
        check(DuplicateHandle(GetCurrentProcess(), slot->OverLapped.hEvent,
            GetCurrentProcess(), &notification, 0, FALSE, DUPLICATE_SAME_ACCESS) != 0,
            "duplicate completion fixture event");
        std::atomic<bool> completed{false};
        std::thread completion([&] {
            Sleep(1200);
            completed = true;
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&slot->OverLapped.Internal), nullptr);
            // Use our own handle: shutdown may close its handle once Internal is complete.
            SetEvent(notification); CloseHandle(notification);
        });
        tuner.CloseTuner(); check(completed, "retain buffer past one-second cancellation timeout");
        completion.join(); check(tuner.m_pIoReqBuff == nullptr, "release completed buffer");
    }
    WSACleanup(); std::cout << "PASS: " << checks << " checks (driver control flow; converter/CAS mocked)\n";
}
