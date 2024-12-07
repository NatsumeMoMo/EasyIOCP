#include "IOCPBase.h"
#include <mstcpip.h>

#pragma comment(lib, "WS2_32.lib")

IOContextPool SocketContext::ioContextPool;		// ��ʼ��

IOCPBase::IOCPBase() :
	completionPort(INVALID_HANDLE_VALUE),
	workerThreads(NULL),
	workerThreadNum(2),
	listenSockContext(NULL),
	fnAcceptEx(NULL),
	fnGetAcceptExSockAddrs(NULL),
	connectCnt(0),
	acceptPostCnt(0)
{
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

#include <iostream>
IOCPBase::~IOCPBase()
{
	Stop();
	WSACleanup();
}


BOOL IOCPBase::Start(std::string ip, int port)
{
	this->IP = ip;

	this->port = port;

	if (false == InitializeIOCP())
		return false;

	if (false == InitializeListenSocket())
	{
		DeInitialize();
		return false;
	}

	return true;
}

void IOCPBase::Stop()
{
	if (listenSockContext != NULL && listenSockContext->connSocket != INVALID_SOCKET)
	{
		// ����ر��¼�
		SetEvent(stopEvent);

		for (int i = 0; i < workerThreadNum; i++)
		{
			// ֪ͨ������ɶ˿��˳�
			PostQueuedCompletionStatus(completionPort, 0, EXIT_CODE, NULL);
		}

		// �ȴ����й����߳��˳�
		WaitForMultipleObjects(workerThreadNum, workerThreads, TRUE, INFINITE);

		// �ͷ�������Դ
		DeInitialize();
	}
}

BOOL IOCPBase::SendData(SocketContext* socketContext, char* data, int size)
{
	if (socketContext == NULL || data == NULL || size <= 0)
		return FALSE;

	// ��������ӵ����Ͷ���
	socketContext->EnqueueSendData(data, size);

	// �����������Ͳ���
	if (socketContext->TryStartSend())
	{
		// ��ȡ���͵�����
		std::vector<char> sendData;
		if (socketContext->GetNextSendData(sendData))
		{
			// ��ȡһ�����͵� IOContext
			IOContext* ioContext = socketContext->GetNewIOContext();
			if (ioContext == nullptr)
				return FALSE;

			// �������ݵ����ͻ�����
			memcpy(ioContext->wsaBuf.buf, sendData.data(), sendData.size());
			ioContext->wsaBuf.len = static_cast<ULONG>(sendData.size());
			ioContext->ioType = SEND_POSTED;
			ioContext->ioSocket = socketContext->connSocket;

			// Ͷ�ݷ�������
			if (!PostSend(socketContext, ioContext))
			{
				socketContext->RemoveContext(ioContext);
				return FALSE;
			}
		}
	}




	return TRUE;
}

std::string IOCPBase::GetLocalIP()
{
	return std::string(DEFAULT_IP);
}

BOOL IOCPBase::InitializeIOCP()
{
	completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (NULL == completionPort)
	{
		return false;
	}

	workerThreadNum = WORKER_THREADS_PER_PROCESSOR * GetNumOfProcessors();
	workerThreads = new HANDLE[workerThreadNum];

	for (int i = 0; i < workerThreadNum; i++)
	{
		workerThreads[i] = CreateThread(0, 0, WorkerThreadProc, (void*)this, 0, 0);
	}
	return true;
}

BOOL IOCPBase::InitializeListenSocket()
{
	// �������ڼ�����socket��Context
	listenSockContext = new SocketContext;
	listenSockContext->connSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == listenSockContext->connSocket)
		return false;

	// ��socket�󶨵���ɶ˿���
	if (NULL == CreateIoCompletionPort((HANDLE)listenSockContext->connSocket, completionPort, (ULONG_PTR)listenSockContext, 0))
	{
		RELEASE_SOCKET(listenSockContext->connSocket);
		return false;
	}

	//��������ַ��Ϣ�����ڰ�socket
	sockaddr_in serverAddr;

	// ����ַ��Ϣ
	ZeroMemory((char*)&serverAddr, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(port);

	// �󶨵�ַ�Ͷ˿�
	if (SOCKET_ERROR == bind(listenSockContext->connSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)))
	{
		return false;
	}

	// ��ʼ����
	if (SOCKET_ERROR == listen(listenSockContext->connSocket, SOMAXCONN))
	{
		return false;
	}

	GUID guidAcceptEx = WSAID_ACCEPTEX;
	GUID guidGetAcceptSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
	// ��ȡ��չ����ָ��
	DWORD dwBytes = 0;
	if (SOCKET_ERROR == WSAIoctl(
		listenSockContext->connSocket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidAcceptEx,
		sizeof(guidAcceptEx),
		&fnAcceptEx,
		sizeof(fnAcceptEx),
		&dwBytes,
		NULL,
		NULL))
	{
		DeInitialize();
		return false;
	}

	if (SOCKET_ERROR == WSAIoctl(
		listenSockContext->connSocket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidGetAcceptSockAddrs,
		sizeof(guidGetAcceptSockAddrs),
		&fnGetAcceptExSockAddrs,
		sizeof(fnGetAcceptExSockAddrs),
		&dwBytes,
		NULL,
		NULL))
	{
		DeInitialize();
		return false;
	}

	for (size_t i = 0; i < MAX_POST_ACCEPT; i++)
	{
		IOContext* ioContext = listenSockContext->GetNewIOContext();
		if (false == PostAccept(listenSockContext, ioContext))
		{
			listenSockContext->RemoveContext(ioContext);
			return false;
		}
	}
	return true;
}

void IOCPBase::DeInitialize()
{
	//ɾ�����¼����
	RELEASE_HANDLE(stopEvent);

	// �ͷŹ������߳̾��ָ��
	for (int i = 0; i < workerThreadNum; i++)
	{
		RELEASE_HANDLE(workerThreads[i]);
	}

	RELEASE(workerThreads);

	// �ر�IOCP���
	RELEASE_HANDLE(completionPort);

	// �رռ���Socket
	RELEASE(listenSockContext);
}

BOOL IOCPBase::IsSocketAlive(SOCKET sock)
{
	int nByteSent = send(sock, "", 0, 0);
	if (-1 == nByteSent)
		return false;
	return true;
}

int IOCPBase::GetNumOfProcessors()
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwNumberOfProcessors;
}

BOOL IOCPBase::AssociateWithIOCP(SocketContext* sockContext)
{
	// �����ںͿͻ���ͨ�ŵ�SOCKET�󶨵���ɶ˿���
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)sockContext->connSocket, completionPort, (ULONG_PTR)sockContext, 0);

	if (NULL == hTemp)
	{
		return false;
	}

	return true;
}

BOOL IOCPBase::PostAccept(SocketContext* sockContext, IOContext* ioContext)
{
	DWORD dwBytes = 0;
	ioContext->ioType = ACCEPT_POSTED;
	ioContext->ioSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == ioContext->ioSocket)
	{
		return false;
	}

	// �����ջ�����Ϊ0,��AcceptExֱ�ӷ���,��ֹ�ܾ����񹥻�
	if (false == fnAcceptEx(listenSockContext->connSocket, ioContext->ioSocket, ioContext->wsaBuf.buf, 0, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &dwBytes, &ioContext->overLapped))
	{
		if (WSA_IO_PENDING != WSAGetLastError())
		{
			return false;
		}
	}

	InterlockedIncrement(&acceptPostCnt);
	return true;
}

BOOL IOCPBase::PostRecv(SocketContext* sockContext, IOContext* ioContext)
{
	DWORD dwFlags = 0, dwBytes = 0;
	ioContext->Reset();
	ioContext->ioType = RECV_POSTED;

	int nBytesRecv = WSARecv(ioContext->ioSocket, &ioContext->wsaBuf, 1, &dwBytes, &dwFlags, &ioContext->overLapped, NULL);
	// �������ֵ���󣬲��Ҵ���Ĵ��벢����Pending�Ļ����Ǿ�˵������ص�����ʧ����
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		DoClose(sockContext);
		return false;
	}
	return true;
}

BOOL IOCPBase::PostSend(SocketContext* sockContext, IOContext* ioContext)
{
	ioContext->ioType = SEND_POSTED;
	DWORD dwBytes = 0, dwFlags = 0;

	// ȷ�����ͻ�������С������ BUFF_SIZE
	if (ioContext->wsaBuf.len > BUFF_SIZE)
		ioContext->wsaBuf.len = BUFF_SIZE;

	if (::WSASend(ioContext->ioSocket, &ioContext->wsaBuf, 1, &dwBytes, dwFlags, &ioContext->overLapped, NULL) != NO_ERROR)
	{
		if (WSAGetLastError() != WSA_IO_PENDING)
		{
			DoClose(sockContext);
			return false;
		}
	}
	return true;
}


BOOL IOCPBase::DoAccpet(SocketContext* sockContext, IOContext* ioContext)
{

	InterlockedIncrement(&connectCnt);
	InterlockedDecrement(&acceptPostCnt);
	SOCKADDR_IN* clientAddr = NULL;
	SOCKADDR_IN* localAddr = NULL;
	int clientAddrLen, localAddrLen;
	clientAddrLen = localAddrLen = sizeof(SOCKADDR_IN);

	// 1. ��ȡ��ַ��Ϣ ��GetAcceptExSockAddrs�����������Ի�ȡ��ַ��Ϣ��������˳��ȡ����һ�����ݣ�
	fnGetAcceptExSockAddrs(ioContext->wsaBuf.buf, 0, localAddrLen, clientAddrLen, (LPSOCKADDR*)&localAddr, &localAddrLen, (LPSOCKADDR*)&clientAddr, &clientAddrLen);

	// 2. Ϊ�����ӽ���һ��SocketContext 
	SocketContext* newSockContext = new SocketContext;
	newSockContext->connSocket = ioContext->ioSocket;
	memcpy_s(&(newSockContext->clientAddr), sizeof(SOCKADDR_IN), clientAddr, sizeof(SOCKADDR_IN));

	// 3. ��listenSocketContext��IOContext ���ú����Ͷ��AcceptEx
	ioContext->Reset();
	if (false == PostAccept(listenSockContext, ioContext))
	{
		listenSockContext->RemoveContext(ioContext);
	}

	// 4. ����socket����ɶ˿ڰ�
	if (NULL == CreateIoCompletionPort((HANDLE)newSockContext->connSocket, completionPort, (ULONG_PTR)newSockContext, 0))
	{
		DWORD dwErr = WSAGetLastError();
		if (dwErr != ERROR_INVALID_PARAMETER)
		{
			DoClose(newSockContext);
			return false;
		}
	}

	//// ������tcp_keepalive
	//tcp_keepalive alive_in;
	//tcp_keepalive alive_out;
	//alive_in.onoff = TRUE;
	//alive_in.keepalivetime = 1000 * 60;  // 60s  �೤ʱ�䣨 ms ��û�����ݾͿ�ʼ send ������
	//alive_in.keepaliveinterval = 1000 * 10; //10s  ÿ���೤ʱ�䣨 ms �� send һ��������
	//unsigned long ulBytesReturn = 0;
	//if (SOCKET_ERROR == WSAIoctl(newSockContext->connSocket, SIO_KEEPALIVE_VALS, &alive_in, sizeof(alive_in), &alive_out, sizeof(alive_out), &ulBytesReturn, NULL, NULL))
	//{
	//	TRACE(L"WSAIoctl failed: %d/n", WSAGetLastError());
	//}


	OnConnectionEstablished(newSockContext);

	// 5. ����recv���������ioContext���������ӵ�socket��Ͷ��recv����
	IOContext* newIoContext = newSockContext->GetNewIOContext();
	newIoContext->ioType = RECV_POSTED;
	newIoContext->ioSocket = newSockContext->connSocket;
	// Ͷ��recv����
	if (false == PostRecv(newSockContext, newIoContext))
	{
		DoClose(sockContext);
		return false;
	}

	return true;
}

BOOL IOCPBase::DoRecv(SocketContext* sockContext, IOContext* ioContext)
{
	OnRecvCompleted(sockContext, ioContext);
	ioContext->Reset();
	if (false == PostRecv(sockContext, ioContext))
	{
		DoClose(sockContext);
		return false;
	}
	return true;
}

BOOL IOCPBase::DoSend(SocketContext* sockContext, IOContext* ioContext)
{

	// ���ûص��������������
	OnSendCompleted(sockContext, ioContext);


	// �ͷŵ�ǰ�� IOContext
	sockContext->RemoveContext(ioContext);

	// ��Ƿ��Ͳ������
	sockContext->FinishSend();

	// ��鲢������һ�����ݿ�
	std::vector<char> nextData;
	if (sockContext->GetNextSendData(nextData))
	{
		// ��ȡһ�����͵� IOContext
		IOContext* newIoContext = sockContext->GetNewIOContext();
		if (newIoContext != nullptr)
		{
			// �������ݵ����ͻ�����
			memcpy(newIoContext->wsaBuf.buf, nextData.data(), nextData.size());
			newIoContext->wsaBuf.len = static_cast<ULONG>(nextData.size());
			newIoContext->ioType = SEND_POSTED;
			newIoContext->ioSocket = sockContext->connSocket;

			// Ͷ�ݷ�������
			if (!PostSend(sockContext, newIoContext))
			{
				sockContext->RemoveContext(newIoContext);
				DoClose(sockContext);
				return FALSE;
			}
		}
	}

	return TRUE;

}

BOOL IOCPBase::DoClose(SocketContext* sockContext)
{
	InterlockedDecrement(&connectCnt);
	RELEASE(sockContext);
	return true;
}

DWORD IOCPBase::WorkerThreadProc(LPVOID lpParam)
{
	IOCPBase* iocp = (IOCPBase*)lpParam;
	OVERLAPPED* ol = NULL;
	SocketContext* sockContext;
	DWORD dwBytes = 0;
	IOContext* ioContext = NULL;

	while (WAIT_OBJECT_0 != WaitForSingleObject(iocp->stopEvent, 0))
	{
		//DWORD�����޸�ΪPULONG_PTR����,����x64ƽ̨
		BOOL bRet = GetQueuedCompletionStatus(iocp->completionPort, &dwBytes, (PULONG_PTR)&sockContext, &ol, INFINITE);

		// ��ȡ����Ĳ���
		ioContext = CONTAINING_RECORD(ol, IOContext, overLapped);

		// �յ��˳���־
		if (EXIT_CODE == sockContext)
		{
			break;
		}
		if (!bRet)
		{
			DWORD dwErr = GetLastError();

			// ����ǳ�ʱ�ˣ����ټ����Ȱ�  
			if (WAIT_TIMEOUT == dwErr)
			{
				//// ȷ�Ͽͻ����Ƿ񻹻���...
				//if (!iocp->IsSocketAlive(sockContext->connSocket))
				//{
				//	iocp->OnConnectionClosed(sockContext);

				//	// ����socket
				//	iocp->DoClose(sockContext);
				//	continue;
				//}
				//else
				//{
				//	continue;
				//}
				iocp->DoClose(sockContext);
			}
			// �����ǿͻ����쳣�˳���(64)
			else if (ERROR_NETNAME_DELETED == dwErr)
			{
				iocp->OnConnectionError(sockContext, dwErr);

				// ����socket
				iocp->DoClose(sockContext);
				continue;
			}
			else
			{
				iocp->OnConnectionError(sockContext, dwErr);

				// ����socket
				iocp->DoClose(sockContext);
				continue;
			}
		}
		else
		{
			// �ж��Ƿ��пͻ��˶Ͽ�
			if ((0 == dwBytes) && (RECV_POSTED == ioContext->ioType || SEND_POSTED == ioContext->ioType))
			{
				iocp->OnConnectionClosed(sockContext);

				// ����socket
				iocp->DoClose(sockContext);
				continue;
			}
			else
			{
				switch (ioContext->ioType)
				{
				case ACCEPT_POSTED:
					iocp->DoAccpet(sockContext, ioContext);
					break;
				case RECV_POSTED:
					iocp->DoRecv(sockContext, ioContext);
					break;
				case SEND_POSTED:
					iocp->DoSend(sockContext, ioContext);
					break;
				default:
					break;
				}
			}
		}
	}

	// �ͷ��̲߳���
	//���߳��ͷŽ��̵�IOCPBase����,����ڴ��쳣
	//RELEASE(lpParam);
	return 0;
}

