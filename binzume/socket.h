#ifndef _SOCKET_H
#define _SOCKET_H
#include<string>


#ifdef _WIN32
#	include<winsock.h>
#	include<FCNTL.h>
#	include<io.h>
	typedef int socklen_t;
#	pragma comment(lib, "ws2_32.lib")
#else
#	include <netdb.h>
#	include <sys/socket.h>
#	include <sys/types.h>
#	include <sys/ioctl.h>
#	include <netinet/in.h>
#	include <netinet/tcp.h>
#	include <arpa/inet.h>
#	include <fcntl.h>
#	include <errno.h>
#	define closesocket close
	typedef int SOCKET;
#endif
#ifndef INVALID_SOCKET
#	define INVALID_SOCKET  (~0)
#endif
#ifndef SOCKET_ERROR
#	define SOCKET_ERROR -1
#endif


// バイトオーダー
#ifndef BYTE_ORDER
#	define LITTLE_ENDIAN 1234
#	define BIG_ENDIAN    4321
#	define PDP_ENDIAN    3412
#	if defined(i386) || defined(_WIN32)
#		define BYTE_ORDER LITTLE_ENDIAN
#	endif
#	ifdef __APPLE__
#		define BYTE_ORDER BIG_ENDIAN
#	endif
#endif
#ifndef BYTE_ORDER
#	pragma message("BYTE_ORDER is not defined! (LITTLE_ENDIAN mode)")
#	define BYTE_ORDER LITTLE_ENDIAN
#endif

// ネットワークバイトオーダ
#ifndef NETWORK_BYTE_ORDER
#	define NETWORK_BYTE_ORDER BIG_ENDIAN
#endif


#ifdef _WIN32
#ifndef NOIMP
class WinsockInit
{
	// singleton
	static WinsockInit instance;
	WinsockInit() {
		WSADATA wsadata;
		WSAStartup(MAKEWORD(1, 1), &wsadata);
	}
	
	~WinsockInit() {
		WSACleanup();
	}
};
WinsockInit WinsockInit::instance;
#endif
#endif

// PDP未対応w
#if NETWORK_BYTE_ORDER != BYTE_ORDER
static inline long NN(long x_){
	unsigned long x = x_;
	x = x >> 16 | x << 16;
	return ((x & 0xff00ff00) >> 8) | ((x&0x00ff00ff) << 8);
}
static inline short NN(short x){
	return (x>>8) | (x<<8);
}
#else
template<T>
static inline T NN(T x){
	return x;
}
#endif


class Socket{
public:
	SOCKET m_socket;
	Socket(const SOCKET &soc){m_socket = soc;}
	// connectTimeoutMs / ioTimeoutMs に正の値を渡すと、それぞれ接続待ちと
	// 送受信待ちに上限を設ける(0以下なら従来通りOS既定のまま待ち続ける)
	Socket(const std::string &host,short port,int connectTimeoutMs=0,int ioTimeoutMs=0){
		m_socket=socket(AF_INET, SOCK_STREAM, 0);
		if (ioTimeoutMs > 0) {
			setIoTimeout(ioTimeoutMs);
		}
		connect(host, port, connectTimeoutMs);
	}
	Socket(){m_socket=socket(AF_INET, SOCK_STREAM, 0);}

	// ノンブロッキングな::connect()が「接続処理を開始した」旨を返したか
	static bool connectInProgress() {
#ifdef _WIN32
		return WSAGetLastError()==WSAEWOULDBLOCK;
#else
		return errno==EINPROGRESS;
#endif
	}

	static bool getaddr(const std::string &host, in_addr *addr) {
		hostent *he;
	    he = gethostbyname(host.c_str());
		if(!he)
			he = gethostbyaddr(host.c_str(), 4, AF_INET);
		if(!he)
		    return false;

		*addr=*(in_addr*)*he->h_addr_list;
		return true;
	}

	static std::string ipstr(const std::string &host){
		std::string s;

		struct hostent *phe = gethostbyname(host.c_str());
		if (!phe) return s;
		s=inet_ntoa(*(in_addr*)(phe->h_addr_list[0]));

		return s;
	}

	static std::string myhostname(){
		std::string s;
		char host[80];
		if (gethostname(host, sizeof(host)) == SOCKET_ERROR) {
			return s;
		}
		s=host;
		return s;
	}

	// ブロッキングモードの切り替え
	bool setBlocking(bool blocking){
		if (m_socket==INVALID_SOCKET)
			return false;
#ifdef _WIN32
		u_long mode = blocking ? 0 : 1;
		return ::ioctlsocket(m_socket, FIONBIO, &mode)==0;
#else
		int flags = ::fcntl(m_socket, F_GETFL, 0);
		if (flags < 0) return false;
		flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
		return ::fcntl(m_socket, F_SETFL, flags)==0;
#endif
	}

	// 送受信のタイムアウトを設定する。到達不能になったサーバーに対して
	// recv()/send()が永久にブロックするのを防ぐ
	bool setIoTimeout(int timeoutMs){
		if (m_socket==INVALID_SOCKET || timeoutMs<=0)
			return false;
#ifdef _WIN32
		// WinsockのSO_RCVTIMEO/SO_SNDTIMEOはミリ秒のDWORDを取る
		int tv = timeoutMs;
#else
		timeval tv;
		tv.tv_sec = timeoutMs/1000;
		tv.tv_usec = (timeoutMs%1000)*1000;
#endif
		bool ok = ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv))==0;
		if (::setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv))!=0) ok = false;
		return ok;
	}

	bool connect(const std::string &host, short port, int timeoutMs=0){
		if(m_socket==INVALID_SOCKET)
			return false;

		sockaddr_in addr;
		in_addr inaddr;

		if (!getaddr(host, &inaddr)) {
			close();
			return false;
		}

		memset (&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr = inaddr;

		if (timeoutMs <= 0) {
			if(::connect(m_socket, (sockaddr*)&addr, sizeof(addr))!=0) {
				close();
				return false;
			}
			return true;
		}

		// 到達不能なホストに対して::connect()はOS既定(Windowsでは20秒程度)まで
		// ブロックする。一時的にノンブロッキングにしてselect()で待つことで、
		// 呼び出し側が指定した時間で見切れるようにする
		if (!setBlocking(false)) {
			close();
			return false;
		}

		bool connected = false;
		if (::connect(m_socket, (sockaddr*)&addr, sizeof(addr))==0) {
			connected = true;
		} else if (connectInProgress()) {
			fd_set wfds, efds;
			FD_ZERO(&wfds); FD_SET(m_socket, &wfds);
			FD_ZERO(&efds); FD_SET(m_socket, &efds);

			timeval tv;
			tv.tv_sec = timeoutMs/1000;
			tv.tv_usec = (timeoutMs%1000)*1000;

			if (select((int)m_socket+1, NULL, &wfds, &efds, &tv) > 0 && !FD_ISSET(m_socket, &efds)) {
				// select()が書き込み可を返しても失敗している場合があるので確認する
				int soerr = 0;
				socklen_t len = sizeof(soerr);
				if (::getsockopt(m_socket, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len)==0 && soerr==0) {
					connected = true;
				}
			}
		}

		if (!connected || !setBlocking(true)) {
			close();
			return false;
		}
		return true;
	}
	void close(){
		if (m_socket!=INVALID_SOCKET) {
			::closesocket(m_socket);
			m_socket=INVALID_SOCKET;
		}
	}

	int send(const void *buf,size_t len){
		int ret=::send(m_socket, (const char*)buf, (int)len, 0);
		if (ret<1) close();
		return ret;
	}
	int recv(void *buf,size_t len){
		int ret=::recv(m_socket, (char*)buf, (int)len, 0);
		if (ret<1) close();
		return ret;
	}


	bool readable() const {
		timeval tv={0,1};
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(m_socket, &fds);
		
		select((int)m_socket+1, &fds, NULL, NULL, &tv);
		
		return FD_ISSET(m_socket, &fds)!=0;
	}
	bool writable() const {
		timeval tv={0,1};
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(m_socket, &fds);
		
		select((int)m_socket+1, NULL, &fds, NULL, &tv);
		
		return FD_ISSET(m_socket, &fds)!=0;
	}

	int write(const std::string &s){
		return send(s.c_str(),s.size());
	}

	std::string& read(std::string &s){
		s.clear();
		char c;
		while (recv(&c,1)) {
			s.push_back(c);
			if ( c=='\0' || c=='\n' ) break;
		}
		return s;
	}
	std::string read(){
		std::string s;
		return read(s);
	}


	int writeInt(long d){
		d=NN(d);
		return send(&d,sizeof(d));
	}
	int writeShort(short d){
		d=NN(d);
		return send(&d,sizeof(d));
	}
	int writeByte(char d){
		return send(&d,sizeof(d));
	}

	int writeStr(const std::string &s){
		return send(s.c_str(),s.size()+1);
	}
	int writeLine(const std::string &s){
		return send(s.c_str(),s.size())+send("\n",1);
	}

	int readInt(){
		long d;
		recv(&d,sizeof(d));
		return NN(d);
	}
	int readShort(){
		short d;
		recv(&d,sizeof(d));
		return NN(d);
	}
	int readByte(){
		char d;
		recv(&d,sizeof(d));
		return d;
	}

	int readStr(std::string &s,char d='\0',int len=0){
		s.clear();
		char c;
		while (recv(&c,1) && c!='\0' && c!=d) {
			s.push_back(c);
			if (s.size()==len) break;
		}
		return 0;
	}
	std::string readStr(char d='\0',int len=0){
		std::string s;
		readStr(s,d,len);
		return s;
	}

	int readLine(std::string &s,int len=0){
		return readStr(s,'\n',len);
	}
	std::string readLine(){
		return readStr('\n');
	}

	bool error() const{
		if (m_socket==INVALID_SOCKET)
			return true;
		return false;
	}
	bool poll() const{return readable();}

	int setsockopt(int optname, const void *optval, socklen_t vallen){
		return ::setsockopt(m_socket, IPPROTO_TCP, optname, (char*)optval, vallen);
	}

	operator SOCKET(){return m_socket;}
};

class SocketServer{
	sockaddr_in m_addr;
public:
	Socket soc;
	SocketServer(int port,int clients=0){
		int f=1;
		setsockopt(soc.m_socket, SOL_SOCKET
			, SO_REUSEADDR, (const char*)&f, sizeof(f));

		memset(&m_addr, 0, sizeof(m_addr));
		m_addr.sin_family=PF_INET;
		m_addr.sin_port=htons(port);

		if(bind(soc.m_socket, (sockaddr*)&m_addr, sizeof(m_addr))!=0) {
			soc.close();
			return;
		}
		
		if(listen(soc.m_socket, clients)!=0){
			soc.close();
			return;
		}
	}

	Socket accept(){
		socklen_t addrsize=sizeof(m_addr);
		return Socket(::accept(soc.m_socket, (sockaddr*)&m_addr, &addrsize));
	}

	bool poll() const{return soc.readable();}
	bool accepted() const{return soc.readable();}
	bool error() const{return soc.error();}
};


#endif

