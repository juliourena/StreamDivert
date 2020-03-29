#include "stdafx.h"
#include "DivertProxy.h"
#include "utils.h"
#include "windivert.h"

/*
* Cleanup completed I/O requests.
*/
static void cleanup(HANDLE ioport, OVERLAPPED *ignore)
{
	OVERLAPPED *overlapped;
	DWORD iolen;
	ULONG_PTR iokey = 0;

	while (GetQueuedCompletionStatus(ioport, &iolen, &iokey, &overlapped, 0))
	{
		if (overlapped != ignore)
		{
			free(overlapped);
		}
	}
}

DivertProxy::DivertProxy(UINT16 localPort, UINT16 proxyPort, std::vector<DIVERT_PROXY_RECORD> proxyRecords)
{
	this->running = false;
	this->localPort = localPort;
	this->localProxyPort = proxyPort;
	this->proxyRecords = proxyRecords;
	this->proxySock = NULL;
}

DivertProxy::~DivertProxy()
{
	if (this->running)
	{
		this->Stop();
	}
}

bool DivertProxy::Start()
{
	WSADATA wsa_data;
	WORD wsa_version = MAKEWORD(2, 2);
	int on = 1;
	struct sockaddr_in addr;

	//lock scope
	{
		std::lock_guard<std::mutex> lock(this->resourceLock);
		std::string selfDesc = this->getStringDesc();
		std::string fiendlyProxyRecordStr = this->getFiendlyProxyRecordsStr();
		info("%s: Start", selfDesc.c_str());
		info("%s: Start divertion of:\n%s", selfDesc.c_str(), fiendlyProxyRecordStr.c_str());
		this->ioPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
		if (this->ioPort == NULL)
		{
			error("%s: failed to create I/O completion port (%d)", selfDesc.c_str(), GetLastError());
			goto failure;
		}

		this->event = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (event == NULL)
		{
			error("%s: failed to create event (%d)", selfDesc.c_str(), GetLastError());
			goto failure;
		}

		this->filterStr = this->generateDivertFilterString();
		info("%s: %s", selfDesc.c_str(), this->filterStr.c_str());
		this->hDivert = WinDivertOpen(this->filterStr.c_str(), WINDIVERT_LAYER_NETWORK, this->priority, 0);
		if (this->hDivert == INVALID_HANDLE_VALUE)
		{
			error("%s: failed to open the WinDivert device (%d)", selfDesc.c_str(), GetLastError());
			goto failure;
		}
		if (CreateIoCompletionPort(this->hDivert, this->ioPort, 0, 0) == NULL)
		{
			error("%s: failed to associate I/O completion port (%d)", selfDesc.c_str(), GetLastError());
			goto failure;
		}

		if (WSAStartup(wsa_version, &wsa_data) != 0)
		{
			error("%s: failed to start WSA (%d)", selfDesc.c_str(), GetLastError());
			goto failure;
		}
		this->proxySock = socket(AF_INET, SOCK_STREAM, 0);
		if (this->proxySock == INVALID_SOCKET)
		{
			error("%s: failed to create socket (%d)", selfDesc.c_str(), WSAGetLastError());
			goto failure;
		}
		if (setsockopt(this->proxySock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(int)) == SOCKET_ERROR)
		{
			error("%s: failed to re-use address (%d)", selfDesc.c_str(), GetLastError());
			goto failure;
		}
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(this->localProxyPort);
		if (::bind(this->proxySock, (SOCKADDR *)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			error("%s: failed to bind socket (%d)", selfDesc.c_str(), WSAGetLastError());
			goto failure;
		}

		if (listen(this->proxySock, 16) == SOCKET_ERROR)
		{
			error("%s: failed to listen socket (%d)", selfDesc.c_str(), WSAGetLastError());
			goto failure;
		}
	}//lock scope

	this->running = true;
	this->proxyThread = std::thread(&DivertProxy::ProxyWorker, this);
	this->divertThread = std::thread(&DivertProxy::DivertWorker, this);	
	return true;

failure:
	this->Stop();
	return false;
}


std::string DivertProxy::getFiendlyProxyRecordsStr()
{
	std::string result;
	for (auto record = this->proxyRecords.begin(); record != this->proxyRecords.end(); ++record)
	{
		result += ipToString(record->srcAddr) + " -> " + std::to_string(this->localPort) + "-" + std::to_string(this->localProxyPort) + " -> " + ipToString(record->forwardAddr) + ":" + std::to_string(record->forwardPort) + "\n";
	}
	return result;
}

std::string DivertProxy::getStringDesc()
{
	return std::string("DivertProxy(" + std::to_string(this->localPort) + ":" + std::to_string(this->localProxyPort) + ")");
}

void DivertProxy::DivertWorker()
{
	OVERLAPPED overlapped;
	OVERLAPPED* poverlapped;
	unsigned char packet[MAXPACKETSIZE];
	WINDIVERT_ADDRESS addr;
	PWINDIVERT_IPHDR ip_header;
	PWINDIVERT_TCPHDR tcp_header;
	UINT packet_len;
	DWORD len;
	std::string selfDesc = this->getStringDesc();

	while (TRUE)
	{
		memset(&overlapped, 0, sizeof(overlapped));
		ResetEvent(this->event);
		overlapped.hEvent = this->event;
		if (!WinDivertRecvEx(this->hDivert, packet, sizeof(packet), 0, &addr, &packet_len, &overlapped))
		{
			DWORD lastErr = GetLastError();
			if (lastErr == ERROR_INVALID_HANDLE || lastErr == ERROR_OPERATION_ABORTED)
			{
				goto end;
			}
			else if (lastErr != ERROR_IO_PENDING)
			{
			read_failed:
				warning("%s: failed to read packet (%d)", selfDesc.c_str(), lastErr);
				continue;
			}			

			// Timeout = 1s
			while (WaitForSingleObject(event, 1000) == WAIT_TIMEOUT)
			{
				cleanup(this->ioPort, &overlapped);
			}
			if (!GetOverlappedResult(this->hDivert, &overlapped, &len, FALSE))
			{
				goto read_failed;
			}
			packet_len = len;
		}
		cleanup(this->ioPort, &overlapped);

		if (!WinDivertHelperParsePacket(packet, packet_len, &ip_header, NULL, NULL, NULL, &tcp_header, NULL, NULL, NULL))
		{
			warning("%s: failed to parse packet (%d)", selfDesc.c_str(), GetLastError());
			continue;
		}

		std::string srcIp = ipToString(ntohl(ip_header->SrcAddr));
		std::string dstIp = ipToString(ntohl(ip_header->DstAddr));
		UINT16 srcPort = ntohs(tcp_header->SrcPort);
		UINT16 dstPort = ntohs(tcp_header->DstPort);
		std::string direction_str = addr.Direction == WINDIVERT_DIRECTION_OUTBOUND ? "OUT" : "IN";
		info("%s: Packet %s:%hu %s:%hu %s", selfDesc.c_str(), srcIp.c_str(), srcPort, dstIp.c_str(), dstPort, direction_str.c_str());

		switch (addr.Direction)
		{
		case WINDIVERT_DIRECTION_OUTBOUND:
			for (auto record = this->proxyRecords.begin(); record != this->proxyRecords.end(); ++record)
			{
				if (ip_header->DstAddr == htonl(record->srcAddr) &&
					tcp_header->SrcPort == htons(this->localProxyPort))
				{
					UINT32 dstAddr = ntohl(ip_header->DstAddr);
					std::string dstAddrStr = ipToString(dstAddr);
					info("%s: Modify packet src -> %s:%hu", selfDesc.c_str(), dstAddrStr.c_str(), this->localPort);

					tcp_header->SrcPort = htons(this->localPort);
				}
			}
			break;

		case WINDIVERT_DIRECTION_INBOUND:
			for (auto record = this->proxyRecords.begin(); record != this->proxyRecords.end(); ++record)
			{
				if (ip_header->SrcAddr == htonl(record->srcAddr) &&
					tcp_header->DstPort == htons(this->localPort))
				{
					UINT32 dstAddr = ntohl(ip_header->DstAddr);
					std::string dstAddrStr = ipToString(dstAddr);
					info("%s: Modify packet dst -> %s:%hu", selfDesc.c_str(), dstAddrStr.c_str(), this->localProxyPort);

					tcp_header->DstPort = htons(this->localProxyPort);
				}
			}
			break;
		}

		WinDivertHelperCalcChecksums(packet, packet_len, &addr, 0);
		poverlapped = (OVERLAPPED *)malloc(sizeof(OVERLAPPED));
		if (poverlapped == NULL)
		{
			error("%s: failed to allocate poverlapped memory", selfDesc.c_str());
		}
		memset(poverlapped, 0, sizeof(OVERLAPPED));
		if (WinDivertSendEx(this->hDivert, packet, packet_len, 0, &addr, NULL, poverlapped))
		{
			continue;
		}
		if (GetLastError() != ERROR_IO_PENDING)
		{
			warning("%s: failed to send packet (%d)", selfDesc.c_str(), GetLastError());
			continue;
		}
	}
end:
	info("%s: DivertWorker exiting", selfDesc.c_str());
	return;
}

void DivertProxy::ProxyWorker()
{
	std::string selfDesc = this->getStringDesc();
	while (true)
	{
		struct sockaddr_in clientSockAddr;
		int size = sizeof(clientSockAddr);
		SOCKET incommingSock = accept(this->proxySock, (SOCKADDR *)&clientSockAddr, &size);
		if (incommingSock == INVALID_SOCKET)
		{
			std::lock_guard<std::mutex> lock(this->resourceLock);
			if (this->running == false)
			{
				goto cleanup;
			}
			warning("%s: failed to accept socket (%d)", selfDesc.c_str(), WSAGetLastError());
			continue;
		}
		std::string srcAddr = ipToString(ntohl(clientSockAddr.sin_addr.S_un.S_addr));
		info("%s: Incoming connection from %s:%hu", selfDesc.c_str(), srcAddr.c_str(), ntohs(clientSockAddr.sin_port));
		ProxyConnectionWorkerData* proxyConnectionWorkerData = new ProxyConnectionWorkerData();
		proxyConnectionWorkerData->clientSock = incommingSock;
		proxyConnectionWorkerData->clientAddr = clientSockAddr;
		std::thread proxyConnectionThread(&DivertProxy::ProxyConnectionWorker, this, proxyConnectionWorkerData);
		proxyConnectionThread.detach();
	}
cleanup:
	if (this->proxySock != NULL)
	{
		closesocket(this->proxySock);
		this->proxySock = NULL;
	}
	info("%s: ProxyWorker exiting", selfDesc.c_str());
}

void DivertProxy::ProxyConnectionWorker(ProxyConnectionWorkerData* proxyConnectionWorkerData)
{
	SOCKET destSock = NULL;
	SOCKET clientSock = proxyConnectionWorkerData->clientSock;
	sockaddr_in clientSockAddr = proxyConnectionWorkerData->clientAddr;
	delete proxyConnectionWorkerData;

	std::string selfDesc = this->getStringDesc();

	DIVERT_PROXY_RECORD proxyRecord;
	UINT32 clientAddr = ntohl(clientSockAddr.sin_addr.S_un.S_addr);
	UINT16 clientSrcPort = ntohs(clientSockAddr.sin_port);
	std::string srcAddr = ipToString(clientAddr);
	bool lookupSuccess = this->findProxyRecordBySrcAddr(clientAddr, proxyRecord);
	if (lookupSuccess)
	{
		struct sockaddr_in destAddr;
		ZeroMemory(&destAddr, sizeof(destAddr));
		destAddr.sin_family = AF_INET;
		destAddr.sin_addr.S_un.S_addr = htonl(proxyRecord.forwardAddr);
		destAddr.sin_port = htons(proxyRecord.forwardPort);
		destSock = socket(AF_INET, SOCK_STREAM, 0);
		if (destSock == INVALID_SOCKET)
		{
			error("%s: failed to create socket (%d)", selfDesc.c_str(), WSAGetLastError());
			goto cleanup;
		}
		std::string forwardAddr = ipToString(proxyRecord.forwardAddr);
		info("%s: Connecting to forward host %s:%hu", selfDesc.c_str(), forwardAddr.c_str(), proxyRecord.forwardPort);
		if (connect(destSock, (SOCKADDR *)&destAddr, sizeof(destAddr)) == SOCKET_ERROR)
		{
			error("%s: failed to connect socket (%d)", selfDesc.c_str(), WSAGetLastError());
			goto cleanup;
		}
		
		info("%s: Starting to route %s:%hu -> %s:%hu", selfDesc.c_str(), srcAddr.c_str(), clientSrcPort, forwardAddr.c_str(), proxyRecord.forwardPort);
		ProxyTunnelWorkerData* tunnelDataA = new ProxyTunnelWorkerData();
		ProxyTunnelWorkerData* tunnelDataB = new ProxyTunnelWorkerData();
		tunnelDataA->sockA = clientSock;
		tunnelDataA->sockAAddr = clientAddr;
		tunnelDataA->sockAPort = clientSrcPort;
		tunnelDataA->sockB = destSock;
		tunnelDataA->sockBAddr = proxyRecord.forwardAddr;
		tunnelDataA->sockBPort = proxyRecord.forwardPort;

		tunnelDataB->sockA = destSock;
		tunnelDataB->sockAAddr = proxyRecord.forwardAddr;
		tunnelDataB->sockAPort = proxyRecord.forwardPort;
		tunnelDataB->sockB = clientSock;
		tunnelDataB->sockBAddr = clientAddr;
		tunnelDataB->sockBPort = clientSrcPort;
		std::thread tunnelThread(&DivertProxy::ProxyTunnelWorker, this, tunnelDataA);
		this->ProxyTunnelWorker(tunnelDataB);
		tunnelThread.join();		
	}	

cleanup:
	if(clientSock != NULL)
		closesocket(clientSock);
	if (destSock != NULL)
		closesocket(destSock);

	info("%s: ProxyConnectionWorker exiting for client %s:%hu", selfDesc.c_str(), srcAddr.c_str(), clientSrcPort);
	return;	
}

void DivertProxy::ProxyTunnelWorker(ProxyTunnelWorkerData* proxyTunnelWorkerData)
{
	SOCKET sockA = proxyTunnelWorkerData->sockA;
	std::string sockAAddrStr = ipToString(proxyTunnelWorkerData->sockAAddr);
	UINT16 sockAPort = proxyTunnelWorkerData->sockAPort;
	SOCKET sockB = proxyTunnelWorkerData->sockB;
	std::string sockBAddrStr = ipToString(proxyTunnelWorkerData->sockBAddr);
	UINT16 sockBPort = proxyTunnelWorkerData->sockBPort;
	delete proxyTunnelWorkerData;
	char buf[8192];
	int recvLen;
	std::string selfDesc = this->getStringDesc();
	while (true)
	{
		recvLen = recv(sockA, buf, sizeof(buf), 0);
		if (recvLen == SOCKET_ERROR)
		{
			warning("%s: failed to recv from socket A(%s:%hu): %d", selfDesc.c_str(), sockAAddrStr.c_str(), sockAPort, WSAGetLastError());
			goto failure;
		}
		if (recvLen == 0)
		{
			shutdown(sockA, SD_RECEIVE);
			shutdown(sockB, SD_SEND);
			goto end; //return
		}

		for (int i = 0; i < recvLen; )
		{
			int sendLen = send(sockB, buf + i, recvLen - i, 0);
			if (sendLen == SOCKET_ERROR)
			{
				warning("%s: failed to send to socket B(%s:%hu): %d", selfDesc.c_str(), sockBAddrStr.c_str(), sockBPort, WSAGetLastError());
				shutdown(sockA, SD_BOTH);
				shutdown(sockB, SD_BOTH);
				goto end; //return
			}
			i += sendLen;
		}
	}

failure:
	shutdown(sockA, SD_BOTH);
	shutdown(sockB, SD_BOTH);
end:
	info("%s: ProxyTunnelWorker(%s:%hu -> %s:%hu) exiting", selfDesc.c_str(), sockAAddrStr.c_str(), sockAPort, sockBAddrStr.c_str(), sockBPort);
}

std::string DivertProxy::generateDivertFilterString()
{
	std::string result = "tcp";
	std::vector<std::string> orExpressions;
	std::string proxyFilterStr = "(tcp.SrcPort == " + std::to_string(this->localProxyPort) + ")";
	orExpressions.push_back(proxyFilterStr);

	for (auto record = this->proxyRecords.begin(); record != this->proxyRecords.end(); ++record)
	{
		std::string recordFilterStr = "(tcp.DstPort == " + std::to_string(this->localPort) + " and ip.SrcAddr == " + ipToString(record->srcAddr) + ")";
		orExpressions.push_back(recordFilterStr);
	}

	result += " and (";
	joinStr(orExpressions, std::string(" or "), result);
	result += ")";
	return result;
}

bool DivertProxy::findProxyRecordBySrcAddr(UINT32 srcAddr, DIVERT_PROXY_RECORD& proxyRecord)
{
	for (auto record = this->proxyRecords.begin(); record != this->proxyRecords.end(); ++record)
	{
		if (record->srcAddr == srcAddr)
		{
			proxyRecord = *record;
			return true;
		}
	}
	return false;
}

bool DivertProxy::Stop()
{
	std::string selfDesc = this->getStringDesc();
	info("%s: Stop", selfDesc.c_str());
	{//lock scope
		std::lock_guard<std::mutex> lock(this->resourceLock);
		this->running = false;
		if (this->hDivert != NULL)
		{
			WinDivertClose(this->hDivert);
			this->hDivert = NULL;
		}
		if (this->proxySock != NULL)
		{
			shutdown(this->proxySock, SD_BOTH);
			closesocket(this->proxySock);
			this->proxySock = NULL;
		}
		if (this->ioPort != NULL)
		{
			CloseHandle(this->ioPort);
			this->ioPort = NULL;
		}
		if (this->event != NULL)
		{
			CloseHandle(this->event);
			this->event = NULL;
		}
	}//lock scope

	if (this->divertThread.joinable())
	{
		this->divertThread.join();
	}
	if (this->proxyThread.joinable())
	{
		this->proxyThread.join();
	}
	
	return true;
}
